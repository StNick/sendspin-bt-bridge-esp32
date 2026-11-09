# Sendspin Bluetooth Bridge for ESP32-S31

Firmware port of [trudenboy/sendspin-bt-bridge](https://github.com/trudenboy/sendspin-bt-bridge)
to the **ESP32-S31**, built with **PlatformIO**. The board appears in
[Music Assistant](https://www.music-assistant.io/) as a Sendspin player and plays to one
Bluetooth (A2DP) speaker or pair of headphones.

The original is a 55k-line Python/Docker application built on BlueZ, PulseAudio, aiosendspin and
Flask. None of that runs on a microcontroller, so this is a re-implementation of its behaviour on
ESP-IDF, not a line-by-line translation:

| Original (Linux)                                   | This port (ESP32-S31)                                                    |
|----------------------------------------------------|--------------------------------------------------------------------------|
| aiosendspin (Python) per speaker subprocess        | [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp) 0.9.5, the official C++ Sendspin library |
| BlueZ + PulseAudio A2DP                            | ESP-IDF Bluedroid A2DP source (LDAC → AAC → SBC preference) + AVRCP    |
| FLAC decode → GStreamer → PulseAudio sink          | sendspin-cpp decodes FLAC/PCM → ring buffer → LDAC/AAC/SBC encoder → A2DP |
| Flask web UI, REST API, SSE log stream             | Built-in web UI on port 80, REST API, log view                           |
| mDNS via zeroconf                                  | ESP-IDF mDNS: `_sendspin._tcp` on port 8928, path `/sendspin`             |
| Reconnect policy (`BT_CHECK_INTERVAL`, backoff, `BT_MAX_RECONNECT_FAILS`) | Same formula: `min(interval · 2^max(0, attempt−3), 300 s)` |
| Idle modes: default / power_save / auto_disconnect / keep_alive | Same four modes, same parameters (keep-alive: 1 s 2 Hz infrasound at amplitude 100, minimum interval 30 s) |
| Per-device static delay, SBC fallback 125 ms, one-shot BlueZ delay report | Sendspin output delay, 125 ms start value, one-shot AVDTP delay report from the speaker |
| `SENDSPIN_PAIRING` (off by default)                | "Require pairing" setting (unpaired servers may play by default)          |
| PulseAudio volume (AVRCP absolute volume)          | AVRCP absolute volume when the speaker supports it, software volume otherwise; speaker-side changes sync back |
| MPRIS: speaker buttons → Music Assistant           | AVRCP passthrough: play/pause/next/previous/stop/volume keys → Sendspin controller commands |
| `LAST_VOLUMES`                                     | Last volume restored after reboot                                        |
| RSSI badge (kernel mgmt Get Conn Info)             | HCI Read RSSI (same BR/EDR value: dB relative to the ideal range)         |
| Player stays visible on a null sink in idle standby, withdrawn when the speaker is lost | `set_available()`: available while connected or in idle standby, unavailable otherwise |
| Optional web password                              | Optional HTTP Basic auth (user `admin`)                                  |
| Update checks                                      | Firmware upload (OTA) from the web UI                                     |

### Not ported, and why

- **Multiple speakers per bridge.** Bluedroid's A2DP source drives one sink at a time. Use one
  board per speaker; they share the same Music Assistant server like several bridge instances do.
- **Music Assistant REST API integration (login, groups API, `VOLUME_VIA_MA`) and Home Assistant
  MQTT/REST integration.** Now-playing, transport and volume come over Sendspin itself (metadata
  and controller roles). The player is visible to Home Assistant through Music Assistant.
- **aptX.** Not implemented. Bluetooth output supports LDAC, AAC-LC and SBC.
- **Latency calibration tools, demo mode, HA add-on packaging, LXC/Docker deployment** have no
  counterpart on a microcontroller.

## Hardware

- An **ESP32-S31** board with **PSRAM** (sendspin-cpp holds about 1.3 MB of audio in external RAM).
  The default target is Espressif's **ESP32-S31-Function-CoreBoard-1** (16 MB flash, 16 MB octal
  PSRAM). Other S31 boards work: pick the matching `board` in `platformio.ini` and set
  `CONFIG_ESPTOOLPY_FLASHSIZE_*` in `sdkconfig.defaults`. The partition table needs 8 MB of flash.
  Without PSRAM the firmware still boots and the web UI explains that Sendspin is disabled.
- Not ESP32-S3: the S3 has no Bluetooth Classic, so it cannot do A2DP. The S31 has BR/EDR.
- A Music Assistant server with the Sendspin provider enabled, on the same network.

## Build and flash

ESP32-S31 support is only in the community [pioarduino](https://github.com/pioarduino/platform-espressif32)
platform (`61.04.00-RC1`, which bundles ESP-IDF 6.1.0). It is pinned in `platformio.ini`; PlatformIO
downloads it, the RISC-V toolchain and the components (versions locked in `dependencies.lock`) on
the first build.

Use **pioarduino Core 6.2.0** (`python -m pip install "pioarduino==6.2.0"`). The upstream
PlatformIO 6.1 core does not meet this platform's requirements.

```bash
pio run                       # build
pio run -t upload             # flash over USB
pio device monitor            # serial log at 115200
```

Updates after the first flash can also be uploaded from the web UI (System → Firmware update,
using `.pio/build/esp32-s31/firmware.bin`).

## First-time setup

1. After flashing, the board opens a Wi-Fi access point **`<name>-setup`**, password **`sendspin`**.
   Join it and open <http://192.168.4.1/>.
2. Under **Settings**, enter your Wi-Fi network and password and press **Save**. The bridge
   restarts and joins your network. (If it later cannot reach that network for 60 s, the setup
   access point comes back so you can fix it.)
3. Open `http://<name>.local/` (or the IP from your router). Put the speaker in pairing mode, press
   **Scan for speakers**, then **Use** next to it. The bridge pairs ("Just Works"; legacy speakers
   get the PIN from Settings, default `0000`) and connects.
4. In Music Assistant the player appears through Sendspin discovery under the configured name.
   If discovery does not work on your network, set **Music Assistant Sendspin URL** to the
   server's Sendspin endpoint (for example `ws://<ma-host>:8927/sendspin`).

## Sync and latency

Bluetooth codec negotiation prefers **LDAC**, then **AAC-LC**
(up to 160 kbit/s), then **SBC**. All use the existing 44.1 kHz, 16-bit PCM pipeline.
LDAC requires stereo; AAC and SBC support mono down-mixing when negotiated. A speaker must
advertise a compatible LDAC endpoint; some speakers require HD audio/LDAC to be enabled in
their companion app. The connection log lists advertised endpoints and LDAC rate/channel masks.
The speaker panel shows the Bluetooth codec and encoder bitrate, separately from the incoming
Sendspin codec. LDAC quality is saved in Settings: High (909 kbit/s), Balanced
(default, 606 kbit/s), or Connection priority (303 kbit/s). Quality changes apply without a reboot.
These are LDAC's 44.1 kHz rates; 990/660/330 kbit/s requires a 48 kHz output pipeline, which
this bridge does not advertise. The bitrate display is the codec rate, not Wi-Fi throughput.
LDAC encoding runs on core 1 and processes a transport packet per wake-up. Its float analysis
uses hardware square roots. `/api/status` includes encoder timing, late-batch, send-retry and
packet-drop counters to distinguish CPU pressure from Bluetooth backpressure.
When upgrading an existing checkout, enable `BT_A2DP_USE_EXTERNAL_CODEC` and
`BT_A2DP_CODEC_AAC_ENABLED` and set `BT_A2DP_SEP_NUM_MAX` to 3 in the generated
`sdkconfig.esp32-s31`, or regenerate it from `sdkconfig.defaults` (preserve any board overrides).
Codec playback, fallback, reconnect and suspend/resume require hardware validation.
After flashing, check LDAC-, AAC- and SBC-capable speakers and verify the displayed codec,
then test volume/mute, power-save resume, reconnect and sync against another Sendspin player.

LDAC uses the pinned Sony/AOSP encoder in `components/libldac` (Apache 2.0; upstream LICENSE
and NOTICE retained). Negotiation and media framing adapt the receiver project's transport
overlay to an A2DP source. CMake compiles checked, generated SDK copies inside the build
directory; it never patches the shared ESP-IDF installation. Header/codec arrays are extended
consistently across Bluedroid and its consumers. Incompatible SDK changes fail configuration.

Bluetooth adds latency that the bridge cannot measure. Each speaker starts with a **static delay**
of 125 ms (the original bridge's SBC fallback). If the speaker sends an AVDTP delay report, that
value replaces it once. Adjust it under **Now playing → Static delay** (or from Music Assistant)
until the speaker lines up with your other players; the value is kept across reboots.

## Notes

- Wi-Fi and Bluetooth share one 2.4 GHz radio. A Bluetooth scan briefly disrupts audio, and a
  weak Wi-Fi signal shows up as dropouts sooner than it would on a Linux host. FLAC is preferred
  over PCM to keep Wi-Fi traffic low.
- Wi-Fi modem sleep stays on: ESP-IDF requires it while Bluetooth is enabled.
- "Disconnect" in the web UI releases the speaker until you press **Reconnect**. Auto-disconnect
  (idle mode) instead wakes the speaker as soon as Music Assistant starts playing.
- **Factory reset** erases settings, the Sendspin identity and pairings, and Bluetooth bonds.

## Status

Host encoder regression checks (requires the installed ESP-IDF headers) run with:

```bash
uv run --with ziglang python tests/run_bt_encoder_host.py
python -m unittest discover -s tests -p test_ldac_overlay.py
```

These use the real LDAC encoder, AAC/SBC and FreeRTOS test doubles, and checked SDK transforms
to verify bitstream framing, buffered PCM accounting, retry ownership, timestamps, resume,
MTU rejection, cleanup and isolation from other projects. They do not validate the Bluetooth radio.

Builds against ESP-IDF 6.1.0 for the ESP32-S31. Boot, 16 MB PSRAM, networking, SBC negotiation
and LDAC playback to an Edifier M60 have been checked on hardware. The optimized 606 kbit/s
mode played smoothly with no late batches, send retries or dropped packets in the observed
session. The 909 kbit/s mode encoded within its CPU budget but caused Bluetooth queue pressure
and sync losses on that setup, so 606 kbit/s remains the default. AAC radio playback and other
speaker combinations still require validation.

## Credits

This is a port; the design, behaviour and defaults come from these projects:

- **[sendspin-bt-bridge](https://github.com/trudenboy/sendspin-bt-bridge)** by Mikhail Nevskiy and
  Loryan Strant, the original Linux bridge this firmware ports
  ([docs](https://trudenboy.github.io/sendspin-bt-bridge/), MIT License). If you run Linux, a
  Raspberry Pi or Home Assistant OS, use the original: it does far more than this firmware.
- **[sendspin-cpp](https://github.com/Sendspin/sendspin-cpp)**, the official C++ Sendspin library
  that does the protocol, decoding and synchronisation here
  ([ESP component](https://components.espressif.com/components/sendspin/sendspin-cpp), Apache 2.0).
- **[Sendspin](https://www.sendspin-audio.com/)** and **[Music Assistant](https://www.music-assistant.io/)**.
- **[ESP-IDF](https://github.com/espressif/esp-idf)**, whose
  [A2DP source example](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/bluedroid/classic_bt/a2dp_source)
  the Bluetooth code follows.
- **[pioarduino](https://github.com/pioarduino/platform-espressif32)**, the PlatformIO platform
  that supports the ESP32-S31.

## License

MIT, see [LICENSE](LICENSE). It keeps the original sendspin-bt-bridge copyright notice.
Dependencies fetched at build time keep their own licenses.

## Layout

```
platformio.ini        pioarduino platform, board, partitions
sdkconfig.defaults    Bluetooth Classic, PSRAM, Sendspin roles, networking
partitions.csv        NVS + two 3 MB OTA slots
src/main.cpp          Sendspin client, idle machine, glue between the parts
src/bt_manager.*      Bluedroid A2DP source, AVRCP, GAP, reconnect policy
src/bt_encoder.*      LDAC/AAC/SBC negotiation, software encoding, paced A2DP delivery
components/libldac/   pinned Sony/AOSP LDAC encoder with upstream license/notice
scripts/              checked project-local Bluedroid source overlay and protocol helpers
src/audio_pipe.*      PCM ring buffer between the Sendspin decoder and Bluetooth
src/wifi_manager.*    Wi-Fi station / setup access point, mDNS
src/web_server.*      REST API; src/web/index.html is the UI
src/app_config.*      settings in NVS
src/nvs_persistence.* sendspin-cpp identity and pairing storage
```
