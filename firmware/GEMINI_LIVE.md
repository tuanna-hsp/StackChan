# Gemini Live AI Agent

This firmware replaces the default AI Agent backend (the xiaozhi server) with the
[Google Gemini Live API](https://ai.google.dev/gemini-api/docs/live). The robot talks to
Gemini directly over a secure WebSocket. No relay server is necessary.

You set the API key, the model, the voice and the system prompt on a web page that
the robot serves on your local network.

## Quick start

1. Get an API key at [aistudio.google.com](https://aistudio.google.com) → **API keys**.
2. Build and flash the firmware. See [Build and flash](#build-and-flash).
3. Connect the robot to Wi-Fi. See [Wi-Fi setup](#wi-fi-setup).
4. Open **AI.AGENT** on the robot. Until you set a key, the screen shows an alert that
   gives the settings page address, for example `http://192.168.1.150/`.
5. Open that address in a browser on the same network. Paste the key and click **Save**.
6. Say **"Hi Stack-chan"**, then talk.

The robot closes the Gemini session after 30 seconds without conversation. Then it
goes back to wake word standby.

## What Gemini can do

- **Talk** in real time with voice in and voice out. The robot shows the transcripts
  of both sides on the screen.
- **Search the web** with Google Search grounding, for example to get the weather or
  the news.
- **Control the robot.** The robot gives all its MCP tools to Gemini as functions:
  head movement, LED color, reminders, and others. The robot runs a tool call locally
  and sends the result back to Gemini. Example: *"Look to the left."*
- **Give the settings page address.** The tool `self.gemini.get_settings_url` lets
  you ask the robot for the page address.

## Settings page

The page is at `http://<robot IP>/`. It has these fields:

| Field | Description |
|---|---|
| Gemini API key | A password field. The page never shows the saved key. Leave it blank to keep the saved key. |
| Model | The default is `models/gemini-3.8-live`. You can omit the `models/` prefix. |
| Voice | Default, Puck, Charon, Kore, Fenrir, Aoede, Leda, Orus or Zephyr. |
| System prompt | The persona and the instructions. The maximum is 3500 characters. |

Changes apply from the next conversation. The robot keeps the settings in NVS
(namespace `gemini`, keys `api_key`, `model`, `voice` and `prompt`).

> **Security:** The page uses plain HTTP and has no login. Anybody on the same
> network can change the settings. Nobody can read the saved key from the page.
> Use the robot only on a network that you trust.

## Wi-Fi setup

The StackChan App's Bluetooth setup does not work with self-built firmware. The App
needs a handshake token from M5Stack's private `secret_logic` library, so the setup
fails with *"could not process device binding response"*.

This firmware uses the xiaozhi **hotspot setup** instead:

1. When the robot has no saved network, it starts a Wi-Fi hotspot.
2. Connect your phone or computer to that hotspot.
3. Select your network on the page that opens, and enter the password.

## Build and flash

Use the normal steps in [README.md](README.md) (ESP-IDF v6.1):

```bash
python3 ./fetch_repos.py
idf.py build
idf.py flash
```

`fetch_repos.py` downloads xiaozhi-esp32 v2.5.2 and applies
`patches/xiaozhi-esp32.patch`. The patch includes the Gemini changes in xiaozhi.

> **Important: the first flash changes the partition table.** The NVS partition
> increased from 16 KB to 64 KB. Thus `otadata` and `phy_init` moved. Do a full
> `idf.py flash` the first time, not `app-flash`. The existing NVS data (Wi-Fi,
> servo calibration and other settings) stays at the same address and stays
> valid.

## How it works

```
mic (16 kHz PCM) ─► AudioService ─► GeminiLiveProtocol ─► realtimeInput  ─►┐
                                                                            │ Gemini Live
speaker ◄─ AudioService ◄─ 24 kHz PCM ◄─ GeminiLiveProtocol ◄─ serverContent ◄┘ (wss)
                                              │  ▲
                              toolCall ─► McpServer (robot tools) ─► toolResponse
```

`GeminiLiveProtocol` implements xiaozhi's `Protocol` interface. Thus the rest of the
application (state machine, wake word, display, audio) works without changes. The
class translates between the two sides:

| xiaozhi | Gemini Live |
|---|---|
| `OpenAudioChannel()` | Opens the WebSocket and sends `setup` (model, voice, system prompt, functions, Google Search, transcription). Waits for `setupComplete`. |
| `SendAudio()` | `realtimeInput.audio`: raw PCM16 at 16 kHz in base64. There is no Opus. |
| `SendStopListening()` | `realtimeInput.audioStreamEnd` |
| `SendAbortSpeaking()` | Gemini has no stop message. The robot drops the remaining audio of the answer. |
| JSON `tts` start, stop and sentence | Made from `modelTurn` audio, `turnComplete` and `outputTranscription`. |
| JSON `stt` | Made from `inputTranscription`. |

The robot sends the API key in the `x-goog-api-key` header, not in the URL. If the
model refuses the transcription settings, the robot connects again without them.

### Files

| File | Purpose |
|---|---|
| `main/hal/gemini/gemini_live.h` | Public interface: config, protocol factory, settings server |
| `main/hal/gemini/gemini_live_protocol.{h,cc}` | The Gemini Live `Protocol` implementation |
| `main/hal/gemini/gemini_config.cc` | Loads and saves settings in NVS, with error reports |
| `main/hal/gemini/gemini_settings_server.cc` | The settings web page (`esp_http_server`) |
| `main/hal/board/stackchan.cc` | Uses the hotspot Wi-Fi setup with Gemini |
| `main/Kconfig.projbuild` | Makes the Wi-Fi setup menu available for StackChan |
| `patches/xiaozhi-esp32.patch` | Changes in xiaozhi (see below) |
| `sdkconfig.defaults`, `partitions.csv` | TLS, memory and network settings (see below) |

### Changes in xiaozhi-esp32

These changes are in `patches/xiaozhi-esp32.patch`:

- **`application.cc`**
  - Uses `gemini_live::create_protocol()` in place of MQTT or WebSocket.
  - Skips the xiaozhi server's version check, activation and OTA. An OTA from that
    server would replace this firmware.
  - Adds a speaking epoch. With it, the protocol knows when the decoder reset at
    the start of the speaking state is complete.
  - Logs an error if the activation task cannot start.
- **`audio_service.{h,cc}`**
  - Adds PCM passthrough. With it, the mic audio goes to the send queue as raw PCM,
    and the robot plays `is_pcm` packets directly, resampled when necessary.
- **`protocol.h`**
  - Adds `AudioStreamPacket::is_pcm`.

## Memory and network tuning

When AI.AGENT runs, the ESP32-S3 has only about 15 KB of fragmented internal RAM.
Most of it goes to the wake word and echo cancellation engine. Each of these
settings fixes a problem that we found during testing:

| Setting | Problem it fixes |
|---|---|
| `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y` | Google serves GTS Root R4 cross-signed by "GlobalSign Root CA". Without this setting, TLS fails with *"No matching trusted root certificate"*. |
| `CONFIG_MBEDTLS_HARDWARE_AES=n` | Hardware AES needs DMA-capable internal RAM chunks. The allocation failed (`esp-aes: Failed to allocate memory`). Software AES is fast enough here. |
| `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` | Wi-Fi and lwIP buffers use PSRAM first. Without this setting, the 7 KB setup message stalled (`SSL send timeout`). |
| `CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=6` (was 16) | Each buffer holds about 1.6 KB of internal RAM. With 16 buffers, the 8 KB activation task could not start, and AI.AGENT stopped at "activating". |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=32768` | The mic upload (about 43 KB/s in base64) does not block the main task. |
| `CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS=2` | Out-of-order segments cannot fill all 6 Wi-Fi RX buffers, which stopped the connection in the middle of an answer. |
| NVS partition 16 KB → 64 KB | The NVS was full. A save of a long system prompt failed. |

The settings page server uses a PSRAM task stack, which saves 8 KB of internal RAM.
Flash access is not permitted from a task with a PSRAM stack. Thus the page reads and
writes NVS on the main task.

Do not increase the TCP receive window or the Wi-Fi RX buffer count. The Wi-Fi RX
buffers stay in internal RAM. When we tried a 16 KB window with 24 RX buffers, the
robot could not connect, and answers stopped in the middle.

## Audio behavior

- **Playback buffer.** Gemini sends audio at about real-time speed in small chunks.
  The robot merges the chunks into 200 ms packets. Each answer starts after 500 ms of
  audio has arrived. The playback queue can hold about 4 s of audio, so small network
  delays do not cause gaps.
- **Half duplex.** The robot does not send mic audio while an answer arrives or
  plays. This frees bandwidth for the answer and prevents the speaker echo from
  interrupting it. As a result, **you cannot interrupt the robot while it speaks**.

## Known limitations

- You cannot interrupt the robot while it speaks (half duplex, see above).
- The StackChan App's Bluetooth Wi-Fi setup and device binding do not work. Use the
  hotspot setup.
- The robot gets no OTA updates from the xiaozhi server.
- Gemini Live sessions have a time limit. When Gemini sends `goAway`, the session ends
  and the next wake word opens a new session. The robot does not resume sessions.
- `gemini_live::is_enabled()` always returns `true`. There is no switch back to the
  xiaozhi server at run time.
