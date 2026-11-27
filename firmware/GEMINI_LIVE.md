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
- **See.** When you ask what it sees, Gemini calls `self.camera.take_photo`. The robot
  takes a photo and sends it to Gemini as a JPEG image (`realtimeInput.video`). Then
  Gemini describes the photo or answers your question about it. Example: *"What am I
  holding?"*
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
| Tool `self.camera.take_photo` | The robot handles this tool itself: it sends the photo as `realtimeInput.video`. The xiaozhi version sends the photo to the xiaozhi vision server. |
| `SendStopListening()` | `realtimeInput.audioStreamEnd` |
| `SendAbortSpeaking()` | Gemini has no stop message. The robot drops the remaining audio of the answer. |
| JSON `tts` start, stop and sentence | Made from `modelTurn` audio, `turnComplete` and `outputTranscription`. |
| JSON `stt` | Made from `inputTranscription`. |

The robot sends the API key in the `x-goog-api-key` header, not in the URL. If the
model refuses the transcription settings, the robot connects again without them.

A separate task (`gemini_send`, with its stack in PSRAM) sends all the messages to
Gemini. Thus a slow upload never blocks the main task, which runs the tool calls and the
state changes. If more than about 3 s of microphone audio waits for upload, the robot
drops the oldest audio. When Gemini starts to answer, the robot drops all the mic audio
that still waits for upload.

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
  - Turns off wake word detection while the robot speaks. Wake word detection also
    turns on echo cancellation (AEC), which uses most of a CPU core. The robot needs
    that CPU time to receive and decrypt Gemini's audio.
- **`audio_service.{h,cc}`**
  - Adds PCM passthrough. With it, the mic audio goes to the send queue as raw PCM,
    and the robot plays `is_pcm` packets directly, resampled when necessary.
  - Puts the stack of the `opus_codec` task (24 KB) in PSRAM. With Gemini, this task
    only decodes the robot's own sounds. This change gives 24 KB of internal RAM to
    Wi-Fi.
- **`protocol.h`**
  - Adds `AudioStreamPacket::is_pcm`.

## Memory and network tuning

When AI.AGENT runs, the ESP32-S3 has very little internal RAM. Wi-Fi needs internal
RAM for its receive buffers. When internal RAM was low, the robot lost packets: a
connection took up to 16 s, the upload stopped for seconds, and answers stuttered.
Each of these changes fixes a problem that we found during testing:

| Change | Problem it fixes |
|---|---|
| `opus_codec` task stack in PSRAM | This change gives 24 KB of internal RAM to Wi-Fi. Before it, free internal RAM fell to 2–6 KB in a session. Now it stays at about 19 KB. |
| `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y` | Google serves GTS Root R4 cross-signed by "GlobalSign Root CA". Without this setting, TLS fails with *"No matching trusted root certificate"*. |
| `CONFIG_MBEDTLS_HARDWARE_AES=n` | Hardware AES needs DMA-capable internal RAM chunks. The allocation failed (`esp-aes: Failed to allocate memory`). |
| `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` | Wi-Fi and lwIP buffers use PSRAM first. Without this setting, the 7 KB setup message stalled (`SSL send timeout`). |
| `CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=6` (was 16) | Each buffer holds about 1.6 KB of internal RAM. With 16 buffers, the 8 KB activation task could not start, and AI.AGENT stopped at "activating". |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10` (was 3), `CONFIG_ESP_WIFI_RX_BA_WIN=6` | More Wi-Fi receive buffers, so the robot loses fewer packets. With 6 buffers, the download still stopped for 1–4 s at a time, and answers stuttered. The window must not be larger than two times the static RX buffers. |
| Display draw buffer 10 lines (was 20) | This change gives 6.4 KB of internal DMA RAM to the extra Wi-Fi receive buffers. |
| `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=12` (was 6), `CONFIG_LWIP_TCP_WND_DEFAULT=12288`, `CONFIG_LWIP_TCP_RECVMBOX_SIZE=12` | Gemini's audio is about 64 KB/s in base64. With the default 5.7 KB window, the download was only about 35 KB/s, and the answers stuttered. |
| `CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS=2` | Out-of-order segments cannot fill all the Wi-Fi RX buffers. When they did, the connection stopped in the middle of an answer. |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=32768` | The mic upload (about 43 KB/s in base64) does not block. |
| NVS partition 16 KB → 64 KB | The NVS was full. A save of a long system prompt failed. |

The settings page server and the `gemini_send` task use PSRAM stacks. Flash access is
not permitted from a task with a PSRAM stack. Thus the page reads and writes NVS on the
main task.

The receive task (`ssl_receive`) decrypts, decodes and parses all of Gemini's messages
at priority 1. During long answers, it can keep a CPU core busy for seconds. It sleeps
for one tick at least every 500 ms, so that the task watchdog does not trigger.

### Diagnostics

Set `kLogStats` to `true` in `gemini_live_protocol.cc` to log these values every 5 s
in a session:

```
Stats 5s: up 84 msgs 215 KB (send avg 6 ms, max 8 ms, dropped 0, queued 0) | down 37 msgs 319 KB, audio 4960 ms
CPU: IDLE1 67% IDLE0 65% ssl_receive 35% taskLVGL 6% wifi 5% ...
```

While the robot speaks, `audio` must be near 5000 ms. If it is lower, the answer
stutters. The CPU line shows the use of each task. Two cores give a total of 200%.

## Audio behavior

- **Playback buffer.** Gemini sends audio at about real-time speed in small chunks.
  The robot merges the chunks into 200 ms packets. Each answer starts after 800 ms of
  audio has arrived. The playback queue can hold about 4 s of audio, so small network
  delays do not cause gaps. If playback runs dry during an answer (on a slow Wi-Fi
  link), the robot buffers 800 ms again before it continues. This gives a few short
  pauses instead of continuous stutter.
- **Wi-Fi access point.** On a mesh network, the robot connects to the access point
  with the strongest signal and stays on it (the `remember_bssid` setting of the
  hotspot setup page, which the firmware turns on by default). Without it, the robot
  often joined a weak access point, and sessions failed or stuttered.
- **Wi-Fi channel width.** The robot uses a 20 MHz channel. With the default 40 MHz,
  it lost many packets on a busy 2.4 GHz band. Gemini needs much less than 1 Mbit/s.
- **Half duplex.** The robot does not send mic audio while an answer arrives or
  plays. This frees bandwidth for the answer and prevents the speaker echo from
  interrupting it. The robot also does not listen for the wake word while it speaks.
  As a result, **you cannot interrupt the robot while it speaks**.

## Known limitations

- You cannot interrupt the robot while it speaks (half duplex, see above).
- The on-screen transcript of your speech sometimes shows the wrong language, for
  example Japanese as Korean. Gemini's own transcription guesses the language. The
  answers are usually correct.
- The StackChan App's Bluetooth Wi-Fi setup and device binding do not work. Use the
  hotspot setup.
- The robot gets no OTA updates from the xiaozhi server.
- Gemini Live sessions have a time limit. When Gemini sends `goAway`, the session ends
  and the next wake word opens a new session. The robot does not resume sessions.
- `gemini_live::is_enabled()` always returns `true`. There is no switch back to the
  xiaozhi server at run time.
