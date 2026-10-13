"""
pc_bridge_tray.py
=================
Install (in the same venv you already use):
    pip install pystray pillow

Run without a console window:
    .venv\\Scripts\\pythonw.exe pc_bridge_tray.py
"""

import asyncio
import json
import os
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

LOG_PATH = os.path.join(HERE, "pc_bridge.log")
SETTINGS_PATH = os.path.join(HERE, "tray_settings.json")
ICON_PATH = os.path.join(HERE, "arc_reactor.png")
APP_NAME = "ArcReactorBridge"


# ---- logging (pythonw has no console, so everything goes to a log file) -------
class _Tee:
    def __init__(self, *streams):
        self.streams = [s for s in streams if s is not None]

    def write(self, text):
        for s in self.streams:
            try:
                s.write(text)
                s.flush()
            except Exception:
                pass
        return len(text)

    def flush(self):
        pass

    def isatty(self):
        return False


def setup_logging():
    try:
        if os.path.exists(LOG_PATH) and os.path.getsize(LOG_PATH) > 1_000_000:
            os.remove(LOG_PATH)
        log = open(LOG_PATH, "a", buffering=1, encoding="utf-8")
        sys.stdout = _Tee(sys.stdout, log)
        sys.stderr = _Tee(sys.stderr, log)
        print("\n--- pc_bridge_tray started ---")
    except Exception:
        pass


setup_logging()

import websockets  # noqa: E402
import pystray  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402
from pystray import Menu, MenuItem as Item  # noqa: E402

import pc_bridge as pb  # noqa: E402


# ---- settings -----------------------------------------------------------------
def load_settings() -> dict:
    try:
        with open(SETTINGS_PATH, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_settings(data: dict):
    try:
        with open(SETTINGS_PATH, "w", encoding="utf-8") as f:
            json.dump(data, f)
    except Exception:
        pass


# ---- icons ----------------------------------------------------------------------
def _fallback_icon() -> Image.Image:
    img = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse((4, 4, 124, 124), outline=(31, 177, 230, 255), width=14)
    d.ellipse((38, 38, 90, 90), fill=(207, 246, 255, 255), outline=(31, 177, 230, 255), width=6)
    return img


def load_icons():
    try:
        on = Image.open(ICON_PATH).convert("RGBA")
    except Exception as e:
        print(f"(could not load {ICON_PATH}: {e}; using a drawn icon)")
        on = _fallback_icon()
    gray = on.convert("L")
    alpha = on.getchannel("A").point(lambda v: int(v * 0.55))
    off = Image.merge("RGBA", (gray, gray, gray, alpha))
    return on, off


# ---- Windows autostart (registry Run key, no extra packages) -------------------------
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"


def autostart_command() -> str:
    exe = sys.executable
    pyw = os.path.join(os.path.dirname(exe), "pythonw.exe")
    if os.path.exists(pyw):
        exe = pyw
    return f'"{exe}" "{os.path.abspath(__file__)}"'


def autostart_enabled() -> bool:
    if sys.platform != "win32":
        return False
    try:
        import winreg

        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            winreg.QueryValueEx(key, APP_NAME)
            return True
    except OSError:
        return False


def set_autostart(enable: bool):
    if sys.platform != "win32":
        return
    import winreg

    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as key:
        if enable:
            winreg.SetValueEx(key, APP_NAME, 0, winreg.REG_SZ, autostart_command())
        else:
            try:
                winreg.DeleteValue(key, APP_NAME)
            except FileNotFoundError:
                pass


# ---- the bridge server, running on its own thread ------------------------------------
class BridgeRunner:
    """Owns the asyncio loop + websocket server so the tray can start/stop it."""

    def __init__(self, on_change, notify):
        self.on_change, self.notify = on_change, notify
        self.loop = asyncio.new_event_loop()
        self.server = None
        self.wake = None
        self.phase = "starting"  # starting | waiting | paused | error | nokey
        self.error = ""
        threading.Thread(target=self._run_loop, daemon=True).start()

    def _run_loop(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def submit(self, coro):
        return asyncio.run_coroutine_threadsafe(coro, self.loop)

    def _set(self, phase, error=""):
        self.phase, self.error = phase, error
        self.on_change()

    async def _handler(self, ws):
        await pb.handle_client(ws, wake=self.wake)

    async def start_server(self):
        if self.server is not None:
            return
        if not (pb.API_KEY or os.environ.get("GEMINI_API_KEY")):
            msg = "No Gemini API key - set GEMINI_API_KEY or add a local .env file"
            self._set("nokey", msg)
            self.notify(msg)
            return
        self._set("starting")
        try:
            if self.wake is None:
                self.wake = await self.loop.run_in_executor(None, pb.WakeDetector)
            self.server = await websockets.serve(
                self._handler, pb.HOST, pb.PORT, max_size=2**23, max_queue=None
            )
            print(f"Listening for ESP32 on ws://{pb.HOST}:{pb.PORT}")
            self._set("waiting")
        except OSError:
            msg = f"Port {pb.PORT} is already in use - is pc_bridge already running?"
            print(msg)
            self._set("error", msg)
            self.notify(msg)
        except Exception as e:
            msg = f"Could not start: {e}"
            print(msg)
            self._set("error", msg)
            self.notify(msg)

    async def stop_server(self):
        server, self.server = self.server, None
        if server is not None:
            server.close()  # also closes the ESP32's connection
            try:
                await asyncio.wait_for(server.wait_closed(), 5)
            except Exception:
                pass
        pb.set_status(connected=False, peer="", state="idle")
        self._set("paused")

    @property
    def running(self) -> bool:
        return self.server is not None


# ---- the tray app ---------------------------------------------------------------------------
STATE_LABELS = {
    "idle": "Idle",
    "listening": "Listening...",
    "thinking": "Thinking...",
    "talking": "Talking",
}


class TrayApp:
    def __init__(self):
        self.settings = load_settings()
        pb.WAKE_ENABLED = bool(self.settings.get("wake", True))
        self.icon_on, self.icon_off = load_icons()
        self._shown = None
        self.icon = pystray.Icon(APP_NAME, self.icon_off, "Arc Reactor", self._build_menu())
        self.runner = BridgeRunner(self.refresh, self.notify)
        pb.STATUS_HOOK = self.refresh

    # -- text shown in the menu / tooltip
    def status_text(self) -> str:
        st = pb.STATUS
        if st["connected"]:
            return f"Connected to ESP32 ({st['peer']})"
        phase = self.runner.phase
        if phase == "starting":
            return "Starting... (loading wake word model)"
        if phase == "waiting":
            return f"Waiting for ESP32... (port {pb.PORT})"
        if phase == "paused":
            return "Disconnected (paused)"
        return self.runner.error or "Not running"

    def tooltip(self) -> str:
        st = pb.STATUS
        text = self.status_text()
        if st["connected"]:
            text += f" - {STATE_LABELS.get(st['state'], st['state'])}"
        return text[:120]

    def refresh(self):
        """Called from the bridge thread whenever status changes."""
        try:
            want = "on" if pb.STATUS["connected"] else "off"
            if want != self._shown:
                self.icon.icon = self.icon_on if want == "on" else self.icon_off
                self._shown = want
            self.icon.title = self.tooltip()
            self.icon.update_menu()
        except Exception:
            pass

    def notify(self, message: str):
        try:
            self.icon.notify(message, "Arc Reactor")
        except Exception:
            pass

    # -- menu actions
    def _toggle_connection(self, icon, item):
        if self.runner.running:
            self.runner.submit(self.runner.stop_server())
        else:
            self.runner.submit(self.runner.start_server())

    def _toggle_wake(self, icon, item):
        pb.WAKE_ENABLED = not pb.WAKE_ENABLED
        self.settings["wake"] = pb.WAKE_ENABLED
        save_settings(self.settings)

    def _toggle_autostart(self, icon, item):
        try:
            set_autostart(not autostart_enabled())
        except Exception as e:
            self.notify(f"Could not change autostart: {e}")

    def _open_log(self, icon, item):
        try:
            os.startfile(LOG_PATH)  # Windows only
        except Exception:
            self.notify(f"Log file: {LOG_PATH}")

    def _show_status(self, icon, item):
        self.notify(self.tooltip())

    def quit(self, icon=None, item=None):
        try:
            self.runner.submit(self.runner.stop_server()).result(timeout=8)
        except Exception:
            pass
        self.icon.stop()

    def _build_menu(self) -> Menu:
        st = pb.STATUS
        return Menu(
            Item(lambda item: self.status_text(), None, enabled=False),
            Item(
                lambda item: f"State: {STATE_LABELS.get(st['state'], st['state'])}",
                None,
                enabled=False,
                visible=lambda item: st["connected"],
            ),
            Menu.SEPARATOR,
            Item(
                lambda item: "Disconnect ESP32" if self.runner.running else "Connect ESP32",
                self._toggle_connection,
            ),
            Item('Wake word "Hey Jarvis"', self._toggle_wake, checked=lambda item: pb.WAKE_ENABLED),
            Item(
                "Start with Windows",
                self._toggle_autostart,
                checked=lambda item: autostart_enabled(),
                visible=sys.platform == "win32",
            ),
            Menu.SEPARATOR,
            Item("Open log", self._open_log),
            Item("Quit", self.quit),
            # hidden item = what a left-click does
            Item("Status", self._show_status, default=True, visible=False),
        )

    def _setup(self, icon):
        icon.visible = True
        self.runner.submit(self.runner.start_server())

    def run(self):
        self.icon.run(setup=self._setup)


def main():
    TrayApp().run()


if __name__ == "__main__":
    main()
