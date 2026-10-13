"""
pc_bridge.py
============
Install:
    pip install google-genai websockets numpy onnxruntime openwakeword

Run:
    set GEMINI_API_KEY=your_key_here        (Windows)
    export GEMINI_API_KEY=your_key_here     (Linux / macOS)
    # or create a local .env file next to this script:
    # GEMINI_API_KEY=your_key_here
    # SYSTEM_PROMPT=You are a helpful assistant.\nAnswer in Greek.
    python pc_bridge.py

For a packaged Windows executable, place .env beside the .exe.
Use \\n in SYSTEM_PROMPT for line breaks.

Tuning (environment variables, all optional):
    WAKE_THRESHOLD   0.5   lower = triggers easier, higher = fewer false wakes
    WAKE_THRESHOLD_TALKING 0.6   same, but used while he is talking (his own voice
                                 leaks into the mic, so be a bit stricter)
    MIN_SPEECH_RMS   400   mic loudness that counts as "you are talking"
    DEBUG_WAKE       1     print wake scores / mic levels to help tune the above
"""

import asyncio
import collections
import json
import os
import sys
import time

import numpy as np
import websockets
from google import genai
from google.genai import types


def _load_dotenv():
    """Load .env beside the script or packaged executable, if present."""
    app_dir = (
        os.path.dirname(sys.executable)
        if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.abspath(__file__))
    )
    env_path = os.path.join(app_dir, ".env")
    try:
        with open(env_path, encoding="utf-8") as handle:
            for line in handle:
                stripped = line.strip()
                if not stripped or stripped.startswith("#") or "=" not in stripped:
                    continue
                key, value = (part.strip() for part in stripped.split("=", 1))
                if key and not os.environ.get(key):
                    os.environ[key] = value.strip().strip("\"'")
    except FileNotFoundError:
        pass
    except OSError as error:
        print(f"WARNING: Could not read .env file at {env_path}: {error}")


_load_dotenv()
API_KEY = os.environ.get("GEMINI_API_KEY", "")

MODEL = "gemini-3.1-flash-live-preview"
MIC_SAMPLE_RATE = 16000      # what the ESP32 sends
SPEAKER_SAMPLE_RATE = 24000  # what Gemini Live returns
HOST = "0.0.0.0"
PORT = 8765

# ---- wake word / turn-taking settings -------------------------------------
WAKE_MODEL = "hey_jarvis"
WAKE_THRESHOLD = float(os.environ.get("WAKE_THRESHOLD", "0.5"))
WAKE_THRESHOLD_TALKING = float(os.environ.get("WAKE_THRESHOLD_TALKING", "0.6"))
MIN_SPEECH_RMS = float(os.environ.get("MIN_SPEECH_RMS", "400"))
DEBUG_WAKE = bool(os.environ.get("DEBUG_WAKE"))

WAKE_SETTLE_S = 0.4        # skip this much audio after the wake word (its tail)
NO_SPEECH_TIMEOUT_S = 6.0  # woke up but you said nothing -> go back to sleep
END_SILENCE_S = 1.0        # this much quiet after you spoke = you're done
MAX_TURN_S = 20.0          # hard cap on one spoken turn
AFTER_REPLY_QUIET_S = 1.0  # ignore the mic right after a reply (speaker echo)
PREROLL_FRAMES = 10        # ~300 ms of audio kept so the first word isn't clipped
SPEECH_NOISE_FACTOR = 2.5  # speech threshold = max(MIN_SPEECH_RMS, noise * this)

DEFAULT_SYSTEM_PROMPT = (
    "You are a helpful, knowledgeable, and reliable AI assistant. Answer in "
    "clear, natural English. Understand the user's request and provide accurate, "
    "practical, and relevant information. Keep answers concise by default, while "
    "providing enough detail for complex topics or when the user asks for it. "
    "Ask a clarifying question when essential information is missing. Be honest "
    "about uncertainty, distinguish facts from assumptions, and never invent "
    "sources or details. Use a respectful, neutral tone and adapt your explanation "
    "to the user's level and context."
)

SYSTEM_PROMPT = (
    os.environ.get("SYSTEM_PROMPT") or DEFAULT_SYSTEM_PROMPT
).replace("\\n", "\n")


# ---- status shared with pc_bridge_tray.py ---------------------------------------------
STATUS = {"connected": False, "peer": "", "state": "idle"}
STATUS_HOOK = None    # the tray sets this to a function it wants called on every change
WAKE_ENABLED = True   # the tray's "Wake word" menu toggle flips this


def set_status(**changes):
    """Update STATUS and tell the tray (if there is one) that something changed."""
    STATUS.update(changes)
    hook = STATUS_HOOK
    if hook is not None:
        try:
            hook()
        except Exception:
            pass


def rms_of(pcm: bytes) -> float:
    if len(pcm) < 2:
        return 0.0
    x = np.frombuffer(pcm[: len(pcm) // 2 * 2], dtype=np.int16).astype(np.float32)
    return float(np.sqrt(np.mean(x * x))) if x.size else 0.0


def _import_openwakeword():
    """Import openwakeword, working around Windows "Application Control" blocks.

    openwakeword's __init__ pulls in scikit-learn (only used for *training* custom
    verifiers, which we never do), and scikit-learn loads scipy.stats' compiled DLLs,
    which some Windows policies block ("DLL load failed ... Application Control").
    If that happens we swap in empty stand-ins for those training-only imports.
    """
    try:
        import openwakeword
        return openwakeword
    except ImportError as e:
        print(f"(openwakeword import failed: {e}; retrying without scikit-learn)")
    import types as _types

    for name in [m for m in sys.modules if m.split(".")[0] in ("openwakeword", "sklearn")]:
        del sys.modules[name]
    stubs = {
        "sklearn": [],
        "sklearn.linear_model": ["LogisticRegression"],
        "sklearn.pipeline": ["make_pipeline"],
        "sklearn.preprocessing": ["FunctionTransformer", "StandardScaler"],
    }
    for name, attrs in stubs.items():
        mod = _types.ModuleType(name)
        for attr in attrs:
            setattr(mod, attr, None)
        sys.modules[name] = mod
    import openwakeword
    return openwakeword


class WakeDetector:
    """openWakeWord wrapper: feed it any amount of 16 kHz PCM16, get the best score."""

    BLOCK_BYTES = 1280 * 2  # openWakeWord wants 80 ms (1280 samples) blocks

    def __init__(self):
        openwakeword = _import_openwakeword()
        from openwakeword.model import Model

        try:
            openwakeword.utils.download_models([WAKE_MODEL])  # no-op if already there
        except Exception as e:  # offline is fine if the files are already cached
            print(f"(wake model download skipped: {e})")
        self.model = Model(wakeword_models=[WAKE_MODEL], inference_framework="onnx")
        self.buf = bytearray()

    def feed(self, pcm: bytes) -> float:
        self.buf += pcm
        best = 0.0
        while len(self.buf) >= self.BLOCK_BYTES:
            block = np.frombuffer(bytes(self.buf[: self.BLOCK_BYTES]), dtype=np.int16)
            del self.buf[: self.BLOCK_BYTES]
            scores = self.model.predict(block)
            if scores:
                best = max(best, max(scores.values()))
        return best

    def reset(self):
        self.buf.clear()
        self.model.reset()


class GeminiLink:
    """Keeps one Gemini Live session open between turns and reconnects if it died."""

    def __init__(self, client, config):
        self.client, self.config = client, config
        self.session = None
        self._cm = None
        self._lock = asyncio.Lock()

    async def get(self):
        async with self._lock:
            if self.session is None:
                cm = self.client.aio.live.connect(model=MODEL, config=self.config)
                self.session = await cm.__aenter__()
                self._cm = cm
            return self.session

    async def drop(self):
        async with self._lock:
            cm, self.session, self._cm = self._cm, None, None
        if cm is not None:
            try:
                await cm.__aexit__(None, None, None)
            except Exception:
                pass


class Bridge:
    """State for one connected ESP32.

    mode:
      idle   - listening for the wake word (audio goes to the detector)
      wake   - wake word heard, waiting for / forwarding your speech
      button - BOOT held, forwarding everything to Gemini
      reply  - Gemini is answering; mic audio only feeds the wake detector so
               "Hey Jarvis" (or the BOOT button) can interrupt
    """

    def __init__(self, ws, link: GeminiLink, wake: WakeDetector):
        self.ws, self.link, self.wake = ws, link, wake
        self.mode = "idle"
        self.noise = 0.0
        self.quiet_until = 0.0
        self.reply_task = None
        self.warm_task = None
        # wake-turn bookkeeping
        self.wake_t0 = 0.0
        self.turn_t0 = 0.0
        self.last_voice = 0.0
        self.speech_started = False
        self.preroll = collections.deque(maxlen=PREROLL_FRAMES)
        self._dbg_t = 0.0

    # ---- helpers ---------------------------------------------------------
    async def send_state(self, state: str):
        set_status(state=state)
        try:
            await self.ws.send(json.dumps({"type": "state", "state": state}))
        except Exception:
            pass

    def speech_threshold(self) -> float:
        return max(MIN_SPEECH_RMS, min(self.noise * SPEECH_NOISE_FACTOR, 3000.0))

    async def go_idle(self, quiet: float = 0.0):
        self.mode = "idle"
        self.speech_started = False
        self.preroll.clear()
        self.quiet_until = time.monotonic() + quiet
        self.wake.reset()
        await self.send_state("idle")

    # ---- Gemini turn control --------------------------------------------
    async def begin_turn(self) -> bool:
        for _ in range(2):  # second try reconnects a session that timed out
            try:
                session = await self.link.get()
                await session.send_realtime_input(activity_start=types.ActivityStart())
                return True
            except Exception as e:
                print(f"[!] Gemini start failed ({e!r}), reconnecting...")
                await self.link.drop()
        await self.go_idle(quiet=1.0)
        return False

    async def send_audio(self, data: bytes) -> bool:
        try:
            await self.link.session.send_realtime_input(
                audio=types.Blob(data=data, mime_type=f"audio/pcm;rate={MIC_SAMPLE_RATE}")
            )
            return True
        except Exception as e:
            print(f"[!] Gemini audio send failed ({e!r})")
            await self.link.drop()
            await self.go_idle(quiet=1.0)
            return False

    async def end_turn(self):
        try:
            await self.link.session.send_realtime_input(activity_end=types.ActivityEnd())
        except Exception as e:
            print(f"[!] Gemini end failed ({e!r})")
            await self.link.drop()
            await self.go_idle(quiet=1.0)
            return
        self.mode = "reply"
        self.speech_started = False
        await self.send_state("thinking")
        self.reply_task = asyncio.create_task(self.stream_reply())

    async def stream_reply(self):
        """Pull Gemini's audio reply and forward it to the ESP32 as it arrives.

        Gemini can hand back one inline_data chunk that's tens of KB, which is
        bigger than the ESP32's WebSocket library accepts in one frame (it closes
        with 1009 "message too big"), so every chunk is re-sliced into small
        fixed-size frames. The ESP32 handles each binary message independently,
        so splitting at any even byte boundary is safe.
        """
        frame_bytes = 4000
        first_chunk = True
        try:
            async for response in self.link.session.receive():
                content = response.server_content
                if not content:
                    continue
                if content.model_turn:
                    for part in content.model_turn.parts:
                        if part.inline_data:
                            if first_chunk:
                                await self.send_state("talking")
                                first_chunk = False
                            data = part.inline_data.data
                            for i in range(0, len(data), frame_bytes):
                                await self.ws.send(data[i : i + frame_bytes])
                if content.turn_complete:
                    break
        except asyncio.CancelledError:
            raise
        except Exception as e:
            print(f"[!] Gemini reply failed ({e!r})")
            await self.link.drop()
        try:
            await self.ws.send(json.dumps({"type": "talk_end"}))
        except Exception:
            pass
        await self.go_idle(quiet=AFTER_REPLY_QUIET_S)

    async def interrupt(self):
        """Stop the current reply (thinking or talking) right now.

        The Gemini session is dropped and reopened for the next turn: that
        guarantees no half-finished reply leaks into the next one, at the cost of
        Gemini forgetting the conversation so far.
        """
        print("[*] Interrupted")
        task, self.reply_task = self.reply_task, None
        if task is not None and not task.done():
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        await self.link.drop()
        try:
            await self.ws.send(json.dumps({"type": "stop_audio"}))
        except Exception:
            pass
        self.mode = "idle"
        self.speech_started = False
        self.preroll.clear()
        self.quiet_until = 0.0
        self.wake.reset()

    # ---- incoming from the ESP32 ----------------------------------------
    async def on_text(self, msg: dict):
        kind = msg.get("type")
        if kind == "listen_start":
            if self.mode == "reply":
                await self.interrupt()  # BOOT pressed while he is thinking/talking
            if self.mode == "idle" or (self.mode == "wake" and not self.speech_started):
                if await self.begin_turn():
                    self.mode = "button"
                    self.preroll.clear()
                    await self.send_state("listening")
            elif self.mode == "wake" and self.speech_started:
                self.mode = "button"  # take over a wake turn already in progress
        elif kind == "listen_end":
            if self.mode == "button":
                await self.end_turn()

    async def on_audio(self, data: bytes):
        now = time.monotonic()
        if self.mode == "idle":
            if now < self.quiet_until:
                return
            level = rms_of(data)
            score = self.wake.feed(data)
            if DEBUG_WAKE and now - self._dbg_t > 0.5:
                self._dbg_t = now
                print(f"  wake score {score:.2f}   mic rms {level:.0f}   noise {self.noise:.0f}")
            if not WAKE_ENABLED:
                return
            if score >= WAKE_THRESHOLD:
                await self.on_wake(now)
            elif score < 0.1:  # slow-moving estimate of the room's background level
                self.noise = level if self.noise == 0 else 0.97 * self.noise + 0.03 * level

        elif self.mode == "wake":
            await self.wake_audio(data, now)

        elif self.mode == "button":
            await self.send_audio(data)
        elif self.mode == "reply":
            # keep listening for "Hey Jarvis" so you can cut him off
            score = self.wake.feed(data)
            if DEBUG_WAKE and now - self._dbg_t > 0.5:
                self._dbg_t = now
                print(f"  (talking) wake score {score:.2f}")
            if WAKE_ENABLED and score >= WAKE_THRESHOLD_TALKING:
                await self.interrupt()
                await self.on_wake(now)

    async def on_wake(self, now: float):
        print("[*] Wake word heard")
        self.mode = "wake"
        self.wake_t0 = now
        self.speech_started = False
        self.preroll.clear()
        # connect to Gemini now (if needed) while you're still talking
        self.warm_task = asyncio.create_task(self._warm())
        await self.send_state("listening")

    async def _warm(self):
        try:
            await self.link.get()
        except Exception:
            pass

    async def wake_audio(self, data: bytes, now: float):
        if now - self.wake_t0 < WAKE_SETTLE_S:
            return
        level = rms_of(data)
        threshold = self.speech_threshold()
        if not self.speech_started:
            self.preroll.append(data)
            if level > threshold:
                if not await self.begin_turn():
                    return
                self.speech_started = True
                self.turn_t0 = self.last_voice = now
                for chunk in list(self.preroll):
                    if not await self.send_audio(chunk):
                        return
                self.preroll.clear()
            elif now - self.wake_t0 > NO_SPEECH_TIMEOUT_S:
                print("[*] No speech after wake word, going back to sleep")
                await self.go_idle(quiet=0.3)
        else:
            if not await self.send_audio(data):
                return
            if level > threshold:
                self.last_voice = now
            if now - self.last_voice > END_SILENCE_S or now - self.turn_t0 > MAX_TURN_S:
                await self.end_turn()

    async def close(self):
        for task in (self.reply_task, self.warm_task):
            if task and not task.done():
                task.cancel()
        await self.link.drop()


async def handle_client(ws, wake=None, client=None):
    print(f"[+] ESP32 connected from {ws.remote_address}")
    peer = ws.remote_address
    set_status(connected=True, peer=f"{peer[0]}" if peer else "", state="idle")

    if client is None:
        api_key = API_KEY or os.environ.get("GEMINI_API_KEY")
        if not api_key:
            print("ERROR: set GEMINI_API_KEY in the environment or create a local .env file "
                  "next to pc_bridge.py.")
            await ws.close()
            return
        client = genai.Client(api_key=api_key)

    config = types.LiveConnectConfig(
        response_modalities=[types.Modality.AUDIO],
        system_instruction=types.Content(parts=[types.Part(text=SYSTEM_PROMPT)]),
        # We decide when a turn starts/ends (BOOT button or wake word + our own
        # VAD), so Gemini's server-side voice detection is switched off.
        realtime_input_config=types.RealtimeInputConfig(
            automatic_activity_detection=types.AutomaticActivityDetection(disabled=True)
        ),
    )

    wake = wake or WakeDetector()
    wake.reset()
    bridge = Bridge(ws, GeminiLink(client, config), wake)
    await bridge.send_state("idle")
    try:
        async for message in ws:
            if isinstance(message, str):
                try:
                    msg = json.loads(message)
                except json.JSONDecodeError:
                    continue
                await bridge.on_text(msg)
            else:
                await bridge.on_audio(message)
    except websockets.ConnectionClosed:
        pass
    finally:
        await bridge.close()
        set_status(connected=False, peer="", state="idle")
        print(f"[-] ESP32 disconnected from {ws.remote_address}")


async def main():
    wake = WakeDetector()  # load the wake-word model once, share across connections
    print(f"Listening for ESP32 on ws://{HOST}:{PORT}  (say \"Hey Jarvis\" or hold BOOT)")

    async def handler(ws):
        await handle_client(ws, wake=wake)

    # max_queue=None: if a turn takes a moment to start (reconnecting to Gemini), the
    # mic frames that arrive meanwhile are queued instead of stalling the ESP32.
    async with websockets.serve(handler, HOST, PORT, max_size=2**23, max_queue=None):
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped.")
        sys.exit(0)