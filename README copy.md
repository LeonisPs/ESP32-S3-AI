# PC Bridge

PC Bridge pairs a Waveshare ESP32-S3-Touch-AMOLED-1.43C with a Windows PC for
voice conversations using Gemini Live. The ESP32 handles the screen, microphone,
speaker, and Wi-Fi. The PC handles wake-word detection and Gemini requests.

## How it works

1. The ESP32 connects to Wi-Fi and discovers the PC bridge on the local network.
2. Microphone audio streams to the PC over WebSocket.
3. The PC waits for the **Hey Jarvis** wake word or a press of the ESP32 BOOT
   button before sending speech to Gemini.
4. Gemini's audio response is sent back to the ESP32 for playback.

The PC bridge listens on TCP port `8765`.

## PC setup

Install Python 3.10 or later, then open PowerShell in this folder and install
the dependencies:

```powershell
py -m pip install google-genai websockets numpy onnxruntime openwakeword pystray pillow
```

Create a local `.env` file next to `pc_bridge.py` (or next to the packaged
`.exe`) using `.env.example` as a template. Set your Gemini API key and, if
desired, a custom system prompt:

```dotenv
GEMINI_API_KEY=replace_with_your_key
SYSTEM_PROMPT=You are a helpful voice assistant.\nSpeak clearly and keep replies concise.
```

The prompt is optional; the application uses its built-in default when
`SYSTEM_PROMPT` is absent or empty. Use the literal `\n` sequence for line
breaks. Do not commit `.env` or put API keys in source code.

### Run from source

Start the system-tray app:

```powershell
py .\pc_bridge_tray.py
```

Or start the bridge server in a console:

```powershell
py .\pc_bridge.py
```

Keep the PC bridge running while using the ESP32. On first start, Windows
Firewall may ask whether Python can accept local network connections; allow
access on your trusted private network.

### Build a Windows tray executable

Install PyInstaller in the same Python environment as the dependencies:

```powershell
py -m pip install pyinstaller
```

Build the tray app from this folder:

```powershell
py -m PyInstaller --noconfirm --clean --onefile --windowed --name ArcReactorBridge --add-data "arc_reactor.png;." --collect-all google.genai --collect-all websockets --collect-all numpy --collect-all onnxruntime --collect-all openwakeword --collect-all pystray --collect-all PIL pc_bridge_tray.py
```

The executable is created at `dist\ArcReactorBridge.exe`. Put `.env` beside
that executable. Rebuild it after changing the Python sources.

## ESP32 setup

1. Install the Waveshare board support and required board components for the
   ESP32-S3-Touch-AMOLED-1.43C using the vendor's Arduino setup instructions.
2. Install the Arduino libraries **WebSockets** by Markus Sattler and
   **ArduinoJson 7.x** by Benoit Blanchon.
3. Open `pc_bridge.ino` in Arduino IDE, select the matching ESP32-S3 board and
   port, then compile and upload.
4. On first boot, connect a phone or computer to the ESP32's `Malaka-...` Wi-Fi
   setup hotspot. The displayed hotspot password is `12345678`. Open
   `http://192.168.4.1/`, enter the home Wi-Fi credentials, and save them.
5. Put the PC and ESP32 on the same local network, then start the PC bridge.

The ESP32 saves Wi-Fi credentials in its preferences storage. To provision a
different network later, use the firmware's Wi-Fi setup control.

The checked-in `CMakeLists.txt` describes the generated SquareLine UI library;
it is not a complete standalone firmware build configuration. Use the
appropriate Waveshare/Arduino board setup to compile the firmware.

## Configuration

| Variable | Purpose | Default |
| --- | --- | --- |
| `GEMINI_API_KEY` | Gemini API key | Required |
| `SYSTEM_PROMPT` | Custom Gemini system instruction | Built-in prompt |
| `WAKE_THRESHOLD` | Wake-word sensitivity while idle | `0.5` |
| `WAKE_THRESHOLD_TALKING` | Wake-word sensitivity while audio is playing | `0.6` |
| `MIN_SPEECH_RMS` | Minimum microphone level treated as speech | `400` |
| `DEBUG_WAKE` | Enable wake detector debug output when set | Off |

The tray menu can also enable or disable wake-word listening. The BOOT
push-to-talk control remains available.

## Security and network notes

- Keep `.env` private. `.gitignore` excludes it; commit only the placeholder
  `.env.example`.
- The bridge uses an unauthenticated, unencrypted WebSocket on port `8765` and
  listens on all network interfaces. Use it only on a trusted LAN. Do not
  expose or port-forward this port to the internet.
- Microphone audio is sent to the PC while connected so local wake-word
  detection can work. Audio is sent to Gemini after a wake-word or push-to-talk
  turn begins.
- Change the setup hotspot password in `pc_bridge.ino` if you need stronger
  protection during Wi-Fi provisioning.

## Project files

- `pc_bridge.py` — WebSocket server, wake-word detection, and Gemini Live client.
- `pc_bridge_tray.py` — Windows system-tray frontend for the server.
- `pc_bridge.ino` — ESP32 firmware.
- `screens/`, `ui.*`, `components/`, `fonts/`, `assets/` — generated LVGL UI.
- `src/`, `esp32_s3_touch_amoled_1.43c.*`, `i2c_bsp.*` — board and audio codec
  support sources.
