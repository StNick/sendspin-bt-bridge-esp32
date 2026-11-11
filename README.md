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
  `CONFIG_ESPTOOLPY_FLASHSIZE_*` in `sdkconfig.defaults`. The partition table needs 16 MB of flash.
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
(up to 256 kbit/s), then **SBC**. Settings select **44.1 or 48 kHz** and **16 or 24 bit**
for Music Assistant's lossless FLAC input (raw PCM fallback). The default remains 44.1 kHz/16-bit.
Changing rate or depth restarts the bridge so Sendspin and all Bluetooth endpoints advertise
one consistent rate. The speaker must support that rate; no rate conversion runs on the board.
LDAC receives packed 16/24-bit stereo; AAC/SBC convert to 16-bit, with mono down-mixing when
negotiated. Selecting 24-bit does not add detail to a 16-bit source.

A speaker must advertise a compatible LDAC endpoint; some require HD audio/LDAC enabled in
their companion app. The connection log lists advertised endpoints and LDAC rate/channel masks.
The speaker panel shows the Bluetooth codec, encoder input format and codec bitrate; the Music
Assistant panel shows the actual incoming codec and resolution. LDAC quality is saved in Settings:

| Quality | 44.1 kHz | 48 kHz |
|---|---:|---:|
| High | 909 kbit/s | 990 kbit/s |
| Balanced (default) | 606 kbit/s | 660 kbit/s |
| Connection priority | 303 kbit/s | 330 kbit/s |
| Off | AAC/SBC only | AAC/SBC only |

Quality changes apply without a reboot. **Off** does not offer LDAC to the speaker at all, so it
negotiates AAC or SBC instead. Use it if a speaker picks LDAC but its encoder fails to start, which
shows up as repeated "encoder failed" reconnects in the log. Turning LDAC off or on restarts the bridge. Higher rates increase shared-radio bandwidth and can
stutter even when encoding is fast enough. The bitrate display is the codec rate, not Wi-Fi throughput.
LDAC encoding runs on core 1 and processes a transport packet per wake-up. Its float analysis
uses hardware square roots. Playback progress reports PCM consumption against a cumulative
sample clock, including encoder priming and congestion losses, to avoid clock drift from delayed
packet feedback. The PCM ring holds 400 ms at every resolution.

`/api/status` includes encoder timing, late batches, send retries, dropped packets, PCM buffer
level and cumulative PCM underrun frames during an active Sendspin stream. Underruns can also
include stream startup/end gaps; compare counter changes during continuous music. Rising PCM
underruns with zero Bluetooth retries/drops suggest upstream delivery or scheduling starvation.
Weak Wi-Fi can cause this; improve reception and try a longer Buffer/Startup lead time or
Connection priority. The maximum buffer is 3000 ms at 16-bit and 2000 ms at 24-bit so it fits
Sendspin's advertised receive capacity. Buffer and lead-time changes restart the bridge.
When upgrading an existing checkout, enable `BT_A2DP_USE_EXTERNAL_CODEC` and
`BT_A2DP_CODEC_AAC_ENABLED` and set `BT_A2DP_SEP_NUM_MAX` to 3 in the generated
`sdkconfig.esp32-s31`, or regenerate it from `sdkconfig.defaults` (preserve any board overrides).
Codec playback, fallback, reconnect and suspend/resume require hardware validation.
After flashing, check LDAC-, AAC- and SBC-capable speakers and verify the displayed codec,
then test volume/mute, power-save resume, reconnect and sync against another Sendspin player.

AAC uses the Fraunhofer FDK AAC encoder (`components/fdk-aac`), the one Android uses. Its
peak-bitrate limit is set from the speaker's MTU, so every AAC frame fits one Bluetooth media
packet; the average target is lowered only when the MTU is too small for 256 kbit/s. Espressif's
encoder, used before, only bounds the average bitrate, so dense music produced frames that had
to be skipped (audible 23 ms gaps). FDK delays audio by about 36 ms at 44.1 kHz; that delay is
reported to Sendspin's sync, so it does not need a manual static delay. FDK's optional
**afterburner** (Settings → "AAC afterburner") gives slightly better AAC at about 2.5× the encoding
CPU. It is off by default: with it on, dense music at 256 kbit/s took up to 30 ms to encode a
23 ms frame on the S31. Changing it restarts only the AAC encoder. FDK's licence grants no
AAC patent rights (see its NOTICE); as with LDAC, a commercial product must handle that itself.

LDAC uses the pinned Sony/AOSP encoder in `components/libldac` (Apache 2.0; upstream LICENSE
and NOTICE retained). Sony's NOTICE says LDAC in a commercial product needs Sony's
[certification](https://www.sony.net/Products/LDAC/aosp/); this is a hobby project, so anyone
building a product from it must handle that themselves. Negotiation and media framing adapt the receiver project's transport
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
uv run --python 3.11 --with ziglang python tests/run_bt_encoder_host.py
python -m unittest discover -s tests -p test_ldac_overlay.py
```

These use the real LDAC and FDK-AAC encoders, SBC and FreeRTOS test doubles, and checked SDK transforms
to verify all four PCM formats and LDAC quality modes, packed PCM conversion, bitstream framing,
consumed-frame accounting, retry ownership, timestamps, resume,
MTU rejection, cleanup and isolation from other projects. They do not validate the Bluetooth radio.

Builds against ESP-IDF 6.1.0 for the ESP32-S31. Boot, 16 MB PSRAM, networking,
SBC negotiation and LDAC playback to an Edifier M60 have been checked on hardware.
Both 44.1 kHz/16-bit at 606 kbit/s and 48 kHz/24-bit FLAC at 660 kbit/s were confirmed
smooth. The final 660 kbit/s capture had no additional PCM underruns, packet drops,
source-queue rejections or missed encoder deadlines after settling. AAC radio playback,
other speakers, and broader sync/suspend/reconnect coverage still require hardware validation.

High LDAC quality remains selectable, but was not reliable with simultaneous Wi-Fi on the
tested setup. At 990 kbit/s, encoding averaged about 0.92 ms per 5.33 ms batch while the
media path drained only about 777 kbit/s and the source FIFO filled. A controlled test with
Wi-Fi stopped sustained about 992 kbit/s including media headers, with zero queue rejections.
This points to coexistence throughput in this SDK/hardware combination; it is not a universal
limit for every S31 or speaker. Balanced remains the default. CPU and octal PSRAM already
run at 320 MHz and 200 MHz; no experimental clock, controller-buffer or radio-policy changes
are included.

### Audio transport diagnostics

`/api/status` includes `bluetooth.transport`: source-queue depth/high-water and packet
counts, L2CAP backlog/drops, media-write failures, controller buffer completions and longest
drain/completion gaps. Normal L2CAP recursion deferrals are counted instead of logged on
every packet. Transport faults are summarized at most once per 10 seconds outside the audio
path; other SDK errors remain logged.

Counters are relaxed-atomic snapshots and can span adjacent packet operations. Controller
completions report released HCI buffers, not proof that the speaker played audio. Compare
counter changes during steady playback: startup, shutdown and reconnection can contribute
counts or long gaps. `POST /api/diagnostics/transport/reset` resets transport counters only;
encoder and PCM-underrun counters retain their existing lifetimes.

Capture status once per second and save the final log with:

```bash
python scripts/capture_audio_diagnostics.py <bridge-ip> --seconds 90 --output .pio/audio-capture
uv run --python 3.11 --with ziglang python tests/run_transport_diag_host.py
```

For password-protected devices, set `SENDSPIN_WEB_PASSWORD` in the environment; the capture
tool uses HTTP Basic auth as `admin` (override with `--username`). Capture files contain device
logs and should be reviewed before sharing.

The UI shows Wi-Fi reception as a rating plus dBm. Bluetooth shows its standard BR/EDR
ideal-range delta in dB and marks absolute dBm unavailable on this S31 controller SDK. No
unsupported vendor RSSI command is sent. The API's historical `rssi_dbm` field remains a
compatibility alias for the delta; new clients should use `rssi_delta_db`.

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
- **[libldac](https://android.googlesource.com/platform/external/libldac/)**, Sony's LDAC encoder
  from the Android Open Source Project (Apache 2.0), vendored in `components/libldac`.
- **[FDK AAC](https://github.com/mstorsjo/fdk-aac)**, the Fraunhofer FDK AAC Codec Library for
  Android (via the fdk-aac standalone mirror); its encoder is vendored in `components/fdk-aac`.
- **[esp_audio_codec](https://components.espressif.com/components/espressif/esp_audio_codec)**,
  Espressif's SBC encoder.
- LDAC and AAC/SBC output, the Bluedroid LDAC transport overlay and the diagnostics were
  contributed and hardware-tested by Simon ([techfoundrynz](https://github.com/techfoundrynz)).

## License

MIT, see [LICENSE](LICENSE). It keeps the original sendspin-bt-bridge copyright notice.
Dependencies keep their own licenses:
- sendspin-cpp and ESP-IDF: Apache 2.0
- the vendored libldac: Apache 2.0, with its LICENSE and NOTICE in `components/libldac`
- the vendored FDK AAC encoder: the Software License for The Fraunhofer FDK AAC Codec Library for
  Android, kept in `components/fdk-aac/NOTICE`. It allows redistribution in source and binary
  form with that notice, and the source must stay available to recipients of binaries (this
  repository does that). It grants no patent licence.
- `espressif/esp_audio_codec`: the Espressif Modified MIT License, which permits use **only with
  Espressif chips**. That is fine for this firmware, but that code cannot be reused on other
  hardware.

## Layout

```
platformio.ini        pioarduino platform, board, partitions
sdkconfig.defaults    Bluetooth Classic, PSRAM, Sendspin roles, networking
partitions.csv        NVS + two 6 MB OTA slots (16 MB flash)
src/main.cpp          Sendspin client, idle machine, glue between the parts
src/bt_manager.*      Bluedroid A2DP source, AVRCP, GAP, reconnect policy
src/bt_encoder.*      LDAC/AAC/SBC negotiation, software encoding, paced A2DP delivery
components/libldac/   pinned Sony/AOSP LDAC encoder with upstream license/notice
components/fdk-aac/   pinned Fraunhofer FDK AAC encoder libraries with upstream NOTICE
scripts/              checked project-local Bluedroid source overlay and protocol helpers
src/audio_pipe.*      PCM ring buffer between the Sendspin decoder and Bluetooth
src/wifi_manager.*    Wi-Fi station / setup access point, mDNS
src/web_server.*      REST API; src/web/index.html is the UI
src/app_config.*      settings in NVS
src/nvs_persistence.* sendspin-cpp identity and pairing storage
```
