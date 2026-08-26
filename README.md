# ESP32-C5 Zigbee → ESPHome converter

A **Zigbee 3.0 coordinator** that converts its paired devices into **ESPHome
native API** entities, on a single ESP32-C5.

No MQTT broker. No zigbee2mqtt. No ZHA. One device, one integration.

## What this does differently

Everything else in this space goes one of two ways: an ESP32 that *becomes* a
Zigbee device for someone else's coordinator ([ESPHome's zigbee
component](https://esphome.io/components/zigbee/),
[luar123/zigbee_esphome](https://github.com/luar123/zigbee_esphome)), or a
coordinator that publishes to **MQTT** (zigbee2mqtt, ZHA). This is the
combination nobody else builds: the coordinator runs here, and the devices it
pairs show up as **ESPHome sub-devices** in Home Assistant.

**The ESP32-C5 is the reason it is worth doing.** Zigbee lives on 2.4 GHz. Every
2.4 GHz-only gateway — ESP32-C6, ESP32-H2, and any USB stick in a Pi on 2.4 GHz
Wi-Fi — competes with itself for the band. The C5 is dual-band, so the uplink
sits on 5 GHz while Zigbee keeps 2.4 GHz:

    Wi-Fi   channel 48, 5 GHz, -57 dBm
    Zigbee  channel 25, 2.4 GHz

C6 and H2 cannot do this at all.

What follows from that:

- **No broker in the path.** Nothing to install, nothing to keep running,
  one less thing that can be down.
- **Zigbee does not depend on the uplink.** The coordinator comes up before
  Wi-Fi. With the network gone, paired devices stay reachable.
- **Devices are real HA devices**, with entities sorted into Controls,
  Configuration and Diagnostic — not MQTT topics plus discovery payloads.
- **8481 device definitions loaded at runtime** from LittleFS, from 525
  brands, replaceable without recompiling.

## Install

Pre-built images are on the [releases
page](https://github.com/moag1000/ESP32-c5-Zigbee2ESPHome/releases). One file,
flashed at offset 0:

```bash
esptool --chip esp32c5 -p /dev/ttyUSB0 write-flash 0x0 \
    esp32c5-zigbee-esphome-<version>-merged.bin
```

That contains bootloader, partition table, firmware and the device database. The
board then brings up a **captive portal** — join the `ESP32-C5-Setup-XXYY`
access point and enter your Wi-Fi credentials.

The portal also takes the **Home Assistant encryption key**, with a 🎲 button
that generates one in your browser. Paste the same value into Home Assistant
when it asks. Nothing needs compiling.

The released images carry **no credentials of any kind**, including no
encryption key — one baked into a public binary would be shared by everybody who
flashed it. `scripts/release.sh` builds from a config with every secret blanked
and refuses to produce anything if it finds one in the output.

The key is stored in NVS and takes precedence over `CONFIG_ESPHOME_NOISE_PSK`,
so a developer build with a key in `sdkconfig.local` still works unchanged.

Updating firmware later without touching the database:

```bash
esptool --chip esp32c5 -p <port> write-flash 0x20000 <version>-app.bin
```

## First run

Written for a first flash, with no toolchain involved — the image from the
releases page is all you need.

**1. Get esptool.**

```bash
pip install esptool        # or: pipx install esptool
```

**2. Find the port.** Plug the board into USB.

| | |
|---|---|
| macOS | `ls /dev/cu.usbmodem*` |
| Linux | `ls /dev/ttyUSB* /dev/ttyACM*` |
| Windows | Device Manager → Ports (COM & LPT) → `COMx` |

Nothing showing up is more often the cable than the board — many USB cables
carry power and no data.

**3. Check what you downloaded.** Every release ships `SHA256SUMS`:

```bash
shasum -a 256 -c SHA256SUMS      # Linux: sha256sum -c SHA256SUMS
```

**4. Flash.**

```bash
esptool --chip esp32c5 -p <port> write-flash 0x0 \
    esp32c5-zigbee-esphome-<version>-merged.bin
```

That is 16 MB and takes a couple of minutes. Most boards drop into download
mode on their own; if esptool cannot connect, hold **BOOT**, tap **RESET**,
let go of **BOOT**, and run it again.

**5. Give it Wi-Fi.** The board knows no network yet, so it opens an **open**
access point named `ESP32-C5-Setup-XXYY` — the last two bytes of its MAC. Join
it and the setup page usually opens by itself; if it does not, go to
**http://192.168.4.1/**.

It asks for three things: the Wi-Fi network, its password, and an **encryption
key** for Home Assistant. The 🎲 button beside the key field generates one in
your browser — keep a copy, Home Assistant wants the same value in the next
step.

**6. Add it to Home Assistant.** The gateway announces itself over mDNS as
`_esphomelib._tcp`, so **Settings → Devices & Services** should already be
offering it as a discovered ESPHome device. If not, add **ESPHome** manually
with the board's IP and port `6053`. Paste the encryption key when asked.

**7. Pair a Zigbee device.** In **Developer Tools → Actions**:

```yaml
action: esphome.esp32c5_gateway_permit_join
data:
  duration: 120
```

Then put the device into pairing mode — usually a long press until it blinks.
It joins as **its own device** in Home Assistant, not as extra entities on the
gateway.

### What the gateway can be told to do

| service | argument | effect |
|---|---|---|
| `permit_join` | `duration` (seconds) | opens the network for new devices |
| `remove_device` | `device` | unpairs a device and forgets it |
| `reconfigure_device` | `device` | interviews the device again |
| `update_converter_db` | `url` | fetches a new device database over HTTP |

Prefix each with `esphome.` and the device name, e.g.
`esphome.esp32c5_gateway_permit_join`.

### Careful: what a full flash erases

`write-flash 0x0` with the merged image writes all 16 MB, and that includes the
partitions holding your settings and the Zigbee network itself:

    nvs         0x9000     Wi-Fi credentials, encryption key
    zb_storage  0x820000   the Zigbee network — every paired device

So re-flashing the merged image starts over: the portal comes back and every
device has to be paired again. To update **only the firmware** and keep all of
it:

```bash
esptool --chip esp32c5 -p <port> write-flash 0x20000 <version>-app.bin
```

### When it does not go to plan

**No `ESP32-C5-Setup-` network appears.** The portal only opens when the
gateway has no working credentials. After an app-only update it still has the
old ones and goes straight online — which is the point. Deliberately: it also
stays away when the stored SSID has connected successfully before, because a
slow access point is not a wrong password. Association here has taken anywhere
from 7 to 372 seconds on the same network; give it a few minutes before
concluding anything.

**Home Assistant does not find it.** mDNS does not cross VLANs or subnets. Add
the integration by hand with the IP and port `6053`.

**It connects, then drops every so often.** Check the antenna first — see the
hardware section. This is a single-SoC design sharing one RF path.

**A device paired but shows few entities.** The gateway matches it against
8481 definitions by manufacturer and model. An unknown device still works
through a generic converter, just with less. `reconfigure_device` re-runs the
interview.

## Hardware

**ESP32-C5 with 8 MB PSRAM and 16 MB flash.** All three matter:

| | why |
|---|---|
| **C5, not C6/H2** | dual-band Wi-Fi. The C5 can put the uplink on 5 GHz and leave 2.4 GHz to Zigbee; C6 and H2 are 2.4 GHz only and share the band with what they are trying to talk to |
| **8 MB PSRAM** | the converter database, the entity table and all of cJSON live there. Without it internal RAM does not fit the firmware |
| **16 MB flash** | two 4 MB app partitions for OTA plus a 7.2 MB LittleFS partition for the device database |

**Recommended module: ESP32-C5-WROOM-1U-N16R8.** The suffixes are the whole
point. `N16` is 16 MB flash, `R8` is 8 MB PSRAM — the two numbers the table
above asks for — and `U` is the variant with a u.FL connector for an **external
antenna** instead of the printed one.

The antenna is not decoration here. Zigbee, Wi-Fi and BLE share a single RF
path on one SoC, and it is the Wi-Fi uplink that pays for it: when the link
drops, Home Assistant loses the gateway while Zigbee carries on underneath. An
external antenna is the cheapest thing that helps, and it is why the `1U` is
worth picking over the `1`.

Reference dev board: **ESP32-C5-DevKitC-1** (the PSRAM variant) — good for
bringing the firmware up, and it carries the same module. Any C5 module with
N16R8 works; check the PSRAM, some C5 boards ship without it.

**Prefer a non-DFS 5 GHz channel** (36, 40, 44, 48). On DFS channel 64 an API
login took 10-28 seconds and frequently timed out; on channel 48 the same login
takes 0.8-2.6 seconds. Espressif documents that the ESP32-C5 cannot detect
radar and cannot vacate DFS channels itself.

**Wiring**: none required for the gateway itself. The optional mmWave presence
sensor (S3KM1110) is UART, pins in `CONFIG_MMWAVE_*`.

## Status, honestly

Hobby project, one board, one developer. Espressif recommends dual-SoC designs
(ESP32-S3 + ESP32-H2) for production Zigbee gateways, and that advice stands:
single-RF-path systems see more packet loss.

**Use it for** learning, experimenting, and a small network you are willing to
tinker with. **Do not use it for** anything you need to just work.

zigbee2mqtt and ZHA are vastly more mature — thousands of contributors, years of
field testing, far more devices. This is a different shape, not a better one.

Measured on hardware (ESP32-C5, ESP-IDF v6.0.2):

| | |
|---|---|
| Wi-Fi association | 25-27 s from boot, 0 failed attempts |
| Devices in converter DB | 8481, from 2555 manufacturer ids, 525 brands |
| Distinct behaviours | 2003 shared profiles |
| Converter DB on flash | 2618 KB of a 7036 KB partition (37 %) |
| Internal heap, steady state | 82 KB free, BLE on |
| Internal heap, low-water mark | 77 KB |
| Test suite | 169 tests, on-device |
| Entity capacity | about 15 Zigbee devices (190 KB table in PSRAM) |
| Source | ~164,000 lines across 293 files in `main/` |

### Known gaps, and they are real

- **Three devices, one network, one developer.** A Fingerbot Plus, an Aqara
  vibration sensor and a Tuya siren. Everything else in the converter database
  is untested here.
- **The converter database update does not finish.** Where it stood before:
  the board read 1200 bytes of the 134381-byte index and gave up. That was a
  bug in this code — `esp_http_client_read()` returns 0 for "nothing ready
  yet", not `-ESP_ERR_HTTP_EAGAIN`, and the read loop treated 0 as the end of
  the body. Fixed, and it took the transfer from 1200 bytes to 133120.

  What remains is below this firmware. With the BLE scanner paused for the
  duration, the server now delivers all 134381 bytes and the board's TCP stack
  acknowledges them — but the application reads only 4320 of them and then
  `select()` reports the socket unreadable for 30 s while lwIP still holds the
  data. Runs vary between 0 and 133120 bytes, the hard failures are
  `ECONNABORTED` from the board's own lwIP, and during one transfer the gateway
  dropped out of Home Assistant altogether. It is contention on a single RF
  path, not a size limit, and it is the same weakness documented above for
  Wi-Fi generally.

  ESP-IDF v6.1-rc1 does not change it; that was measured, not assumed.

  Flashing `converters-<version>.bin` to `0x921000` works and is the way to
  update the database today.

- **(historical, now fixed) The update worked for small files only.** `update_converter_db` now reports what it is doing through the
  "Converter DB Update" sensor, and against an instrumented server the failure
  is precise: a 16 KB index is fetched, parsed and installed correctly, while
  the real 134 KB index gets 1200 bytes — one segment — and then

      tcp_read error, errno=Software caused connection abort
      esp_transport_read returned:-2 and errno:113

  ECONNABORTED comes from the board's own lwIP, not from the network: the
  server had already written the whole file. Two contributing causes are known.
  `CONFIG_LWIP_MAX_SOCKETS` was 8, which an API server with several clients,
  MQTT and mDNS very nearly exhausts on its own — raised to 16, and that alone
  turned the 16 KB case from failing into working. What remains is lwIP running
  out of buffers on a sustained receive, and it is not fixed.

  Flashing `converters-<version>.bin` to `0x921000` works and is the way to
  update the database today.
- **Tuya devices were write-only until recently, and nothing said so.** Their
  datapoint reports were parsed against the wrong header and then handed to a
  driver table that only the Fingerbot is in, so every other Tuya device could
  be commanded but never reported anything back. Commands went out as `sendData`
  (0x04) for all of them, which is what one Fingerbot needs and what a siren
  acknowledges and ignores. Both are fixed and both were invisible: the device
  answers `OK` either way.
- **The C5 loses its access point periodically and does not find it again.**
  Reason 201, NO_AP_FOUND, with the AP sitting there at -50 dBm. Seen on both a
  DFS and a non-DFS channel, so it is not only a DFS effect. The watchdog
  recovers it — a driver restart, then a reboot after fifteen minutes — and that
  has now brought the gateway back three times without anyone touching it. It is
  a mitigation, not a fix; the fault is below this firmware.
- **The Wi-Fi escalation has been seen through once, end to end.** The gateway
  lost its access point, the driver restart at five minutes changed nothing, and
  the reboot that follows brought it back on its own — boot counter 32 to 33,
  reset reason `software`, roughly eleven minutes off the network with nobody
  touching it. One observation, not a guarantee.
- **App OTA rollback is new and has been exercised once.** The bootloader now
  reverts an image that does not confirm itself, and the firmware confirms once
  Wi-Fi is up and the ESPHome API is listening. Enabling it needed that
  confirmation first: the only one on a boot path sat behind HTTP OTA being
  configured, so turning rollback on without it would have made every image
  pushed over the ESPHome OTA port revert at the next restart — an update that
  looked like it worked and quietly undid itself.
- **GATT initializes but has not been tested** against a peripheral.
- **Most testing runs in minutes.** Both of the worst faults found so far needed
  conditions a short test does not produce: one took nine hours of uptime, the
  other three simultaneous clients. Users produce both without trying.
- The C5's Wi-Fi scan returns nothing useful, so anything scan-based (the
  captive portal's network list) is unreliable.
- **Half the codebase cannot run.** Of the functions defined in files that are
  compiled unconditionally, 820 of 1884 are discarded by the linker as
  unreachable — measured, not estimated, by comparing definitions against the
  symbols in the linked image. Fifteen files that were compiled and dropped
  in their entirety have since been taken out of the build — the sources remain,
  with a note in `main/CMakeLists.txt` saying what they were and how to bring
  them back.

  They were superseded rather than unfinished. The per-device BLE decoders
  (Ruuvi, SwitchBot, Qingping, Inkbird, iBeacon/Eddystone) duplicate what the
  ESPHome Bluetooth proxy already does better: it forwards raw advertisements to
  Home Assistant, whose integrations know far more device types and are updated
  without reflashing. `zb_cluster_measurement.c` allocated illuminance, pressure
  and PM2.5 buffers nothing read, while the clusters themselves are handled by
  `zb_callbacks.c` and the converter. `zb_diagnostics.c` incremented a second
  reset counter into NVS on every boot that nothing reported, next to the Boot
  Count that works.

  This is not merely untidy. Log forwarding to Home Assistant was exactly this
  — a complete implementation nothing called — and it was invisible until
  somebody went looking. Anything in that half may be equally broken and equally
  silent.

  The modules behind `CONFIG_ZB_SCENES_ENABLE`, `CONFIG_ZB_TOUCHLINK_ENABLE`,
  `CONFIG_ZB_OTA_ENABLE` and `CONFIG_ZIGBEE_MULTI_PAN_ENABLED` are *not* in this
  count: they are conditionally compiled and genuinely off, which is a different
  thing from unreachable.

- A good deal of the codebase has never been read.

## How it compares

| | this project | zigbee2mqtt / ZHA | ESPHome zigbee component |
|---|---|---|---|
| Role | Zigbee **coordinator** | coordinator | Zigbee **end device** |
| Path to Home Assistant | ESPHome native API | MQTT broker / ZHA integration | via a separate coordinator |
| Broker required | no | yes (z2m) | no |
| Runs on | ESP32-C5 alone | Pi/server + USB stick | ESP32-C6/H2/C5 |
| Wi-Fi band vs Zigbee | **5 GHz uplink, 2.4 GHz Zigbee** | depends on host | shares 2.4 GHz |
| Device definitions | 8481, runtime-loadable | thousands, mature | n/a |
| Maturity | hobby project | production, years of it | official ESPHome |

## Features

**Zigbee** — 3.0 coordinator, device interview, converter binding, groups,
direct binding, network topology and heal, availability tracking, backup and
restore. Scenes, Touchlink and Zigbee OTA are complete but off by default
(`CONFIG_ZB_SCENES_ENABLE`, `CONFIG_ZB_TOUCHLINK_ENABLE`, `CONFIG_ZB_OTA_ENABLE`).

**Tuya** — the 0xEF00 datapoint protocol in both directions. Datapoint maps come
from both sources upstream keeps them in: the declarative `meta.tuyaDatapoints`
tables and the hand-written converters in `legacy.ts`. 672 of the 2003 shared
profiles carry one, 847 of their enums have names rather than raw numbers, and
513 exposes keep the endpoint suffix that tells a three-gang switch's gangs
apart. Writes go out as `dataRequest`, matching zigbee-herdsman-converters, with
`sendData` kept for the devices measured to need it.

**Link quality is measured, not inferred.** LQI and RSSI come from the stack's
neighbour table, so an unheard device reads `unknown` rather than a confident
-100 dBm.

**Home Assistant** — ESPHome native API on port 6053 with Noise encryption,
sub-devices, 15 entity types, entity categories, app OTA on port 3232, and the
service calls listed above.

**Bluetooth** — NimBLE scanner, passive or active, switchable from Home
Assistant at runtime. Re-enabled after the original reason for disabling it
turned out to be a Wi-Fi coexistence bug rather than a BLE problem.

**MQTT** — optional and secondary. Bridge management and diagnostics. Not
required for Home Assistant, and off the critical path; the whole point of the
project is that you do not need it.

**Other** — S3KM1110 mmWave presence over UART, captive portal for Wi-Fi setup,
crash reporting, LED status, performance metrics.

## The converter database

Pairing gives you an address. Turning that into "Fingerbot Plus, with a mode
selector and a sustain time" takes a device database, and that is the part this
project did not invent — it is distilled from
[zigbee-herdsman-converters](https://github.com/Koenkk/zigbee-herdsman-converters)
and [zha-device-handlers](https://github.com/zigpy/zha-device-handlers).

**It is deduplicated.** 8481 devices describe 2003 distinct behaviours, because
the same hardware is sold under many manufacturer ids. Each device carries its
identity and a profile id; the behaviour is stored once in `profiles_N.json`,
where `N = id / 256`. That is 2618 KB of the 7036 KB partition instead of
6876 KB, and it is what made room for the datapoint maps.

The extraction tooling lives in `tools/` and `scripts/`:

| | |
|---|---|
| `tools/z2m_converter_extract.py` | parses the TypeScript converters |
| `tools/merge_z2m_extracts.py` | combines two extraction runs, keeping the richer entry |
| `tools/merge_converter_dbs.py` | merges z2m and zhaquirks, builds the shared profiles |
| `tools/validate_converter_db.py` | checks every profile reference resolves |
| `scripts/build_converter_db.sh` | builds the LittleFS image |

Rebuilding the database does not require rebuilding the firmware. Write the
image on its own:

```bash
esptool --chip esp32c5 -p <port> write-flash 0x921000 converters-<version>.bin
```

## Building from source

Only needed if you want to change the firmware — the releases are ready to
flash.

### ESP-IDF v6.0.2

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v6.0.2 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6
cd esp-idf-v6
./install.sh esp32c5
alias get_idf='source $HOME/esp/esp-idf-v6/export.sh'
```

v6.0.2 is the current stable release; the v5.5 line is older despite the higher
patch numbers, and v6.1 is still a release candidate.

The Python virtualenv that `install.sh` creates is named after the **host**
Python version (e.g. `idf6.0_py3.14_env`). Upgrading the system Python
invalidates it: `export.sh` still exits 0, but `idf.py` is then not on the PATH
and the log says `ESP-IDF Python virtual environment ... not found`. Re-run
`./install.sh esp32c5` to fix it.

### ESP-Zigbee-SDK

```bash
cd ~/esp
git clone --recursive https://github.com/espressif/esp-zigbee-sdk.git
export ESP_ZIGBEE_SDK_PATH=$HOME/esp/esp-zigbee-sdk
```

### Build, flash, test

```bash
git clone https://github.com/moag1000/ESP32-c5-Zigbee2ESPHome.git
cd ESP32-c5-Zigbee2ESPHome
source ./scripts/setup_env.sh

./scripts/build.sh
./scripts/flash.sh            # or: ./scripts/flash.sh /dev/ttyUSB0
./scripts/monitor.sh
./scripts/run_tests.sh        # 169 tests, run on the device
```

Machine-specific settings — Wi-Fi credentials, the Noise key — go in
`sdkconfig.local`, which is gitignored and picked up automatically by
`CMakeLists.txt`. Never commit it.

Building a publishable image:

```bash
scripts/release.sh v0.3.1
```

That blanks every credential, builds, and then greps the result for your own
secrets before producing anything. If it finds one, it stops.

## Partitions and memory

16 MB flash, dual OTA slots, LittleFS for the database:

| partition | offset | size | purpose |
|---|---|---|---|
| `nvs` | 0x9000 | 24 KB | Wi-Fi credentials, encryption key, settings |
| `otadata` | 0xF000 | 8 KB | which app slot is active |
| `phy_init` | 0x11000 | 4 KB | PHY calibration |
| `ota_0` | 0x20000 | 4 MB | application slot 0 |
| `ota_1` | 0x420000 | 4 MB | application slot 1 (OTA target) |
| `zb_storage` | 0x820000 | 1 MB | the Zigbee network — paired devices |
| `zb_fct` | 0x920000 | 4 KB | Zigbee factory data |
| `spiffs` | 0x921000 | 7036 KB | LittleFS, the converter database |

Internal SRAM is the scarce resource, not flash. Measured steady state:

| configuration | free internal heap | low-water mark |
|---|---|---|
| Zigbee only (2026-08-05) | 122 KB | 121 KB |
| Zigbee + BLE (2026-08-05) | 65 KB | 64 KB |
| Zigbee + BLE, current | 82 KB | 77 KB |

The low-water mark used to be 30 KB regardless of the steady-state figure,
because the tightest moment is early boot rather than operation — loading the
converter index briefly took 158 KB. That allocation now goes to PSRAM, along
with the entity table and all of cJSON.

PSRAM is 8 MB and still barely used, so it is the place to move anything large
that is not touched from an ISR. What is holding internal RAM:

```bash
riscv32-esp-elf-nm --print-size --size-sort --radix=d build/*.elf \
  | awk '$3=="b" || $3=="B" {print $2, $4}' | sort -rn | head -20
```

## Project layout

```
main/
├── zigbee/        coordinator, interview, converters, Tuya, topology (53 .c)
├── core/          bridge, config, adapters, monitoring (43 .c)
├── bluetooth/     NimBLE scanner and GATT client (16 .c)
├── esphome/       native API server, entities, services (14 .c)
├── ota/           app OTA and converter database OTA (4 .c)
├── mqtt/          optional MQTT client (4 .c)
├── wifi/          manager, coexistence, captive portal (3 .c)
├── led/  mmwave/  utils/
data/converters_merged/   the device database, 39 JSON files
tools/                    database extraction and validation
scripts/                  build, flash, monitor, test, release
tests/                    on-device unit and integration tests
docs/                     23 documents
```

## Configuration

Everything is under `idf.py menuconfig` → **ESP32-C5 Zigbee Gateway
Configuration**.

| group | what matters |
|---|---|
| **Wi-Fi** | SSID and password, WPA/WPA2/WPA3, `CONFIG_WIFI_PREFER_5GHZ` with an RSSI adjustment so a dual-band SSID picks 5 GHz |
| **Zigbee** | PAN id (default 0x1A62), channel 11-26 — 15, 20 or 25 avoid Wi-Fi overlap — and the network key |
| **ESPHome** | port 6053, device name, friendly name, Noise key (leave blank and set it through the portal) |
| **Bluetooth** | `CONFIG_BT_SCANNER_ENABLED`, `CONFIG_BT_PROXY_ENABLED`, scan interval |
| **Coexistence** | Wi-Fi priority (recommended), balanced, BT priority, Zigbee priority |
| **MQTT** | optional; broker URL, credentials, topic base |

## Credit where it is due

This gateway would be a Zigbee adapter without two projects that did the hard
part — knowing what a device *is*:

- **[zigbee-herdsman-converters](https://github.com/Koenkk/zigbee-herdsman-converters)**
  (MIT, Koen Kanters) — thousands of devices described by hundreds of people,
  each entry usually somebody buying hardware and working out what its vendor
  actually did. It is why a paired address becomes a "Fingerbot Plus" with a
  mode selector. **[Sponsor Koen](https://github.com/sponsors/Koenkk)** — if
  this project is useful to you, that is where the money should go first.
- **[zha-device-handlers](https://github.com/zigpy/zha-device-handlers)**
  (Apache-2.0) — covers what the z2m set does not.
- **[ESPHome](https://github.com/esphome/esphome)** — the native API's wire
  format, message numbering and entity model are theirs.

Full notices in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## How this was built

The code was written by an AI assistant (Claude). The direction was not.

Every design decision here came from someone actually running a smart home and
being annoyed by something. The central one:

> MQTT is a fine vehicle. But going straight into ESPHome — into Home
> Assistant — is so much more practical.

That is the entire premise, and it came from use, not from architecture
astronomy. So did most of what followed: pairing a Fingerbot and wanting its
*settings* in Home Assistant rather than just an on/off switch; noticing that a
one-second sustain time held the switch for ten; asking why Wi-Fi took minutes
when every other device in the house connects instantly — which turned out to
be a coexistence call in the wrong place and is now the single biggest fix in
the project.

Several conclusions in this repository were wrong until a practical objection
overturned them. The Wi-Fi problem was diagnosed as broken hardware and closed;
one sentence — "Wi-Fi works, just often not immediately" — reopened it and led
to the actual cause. Bluetooth was disabled for a reason that turned out to be a
symptom of that same bug.

If you find this useful, the credit for it being *the right thing to build*
belongs to the person who kept asking why.

## Contributing

Issues and pull requests are welcome. Coding standards are in
[docs/CODE_STYLE.md](docs/CODE_STYLE.md); add tests under `tests/` for anything
new, and run `./scripts/run_tests.sh` on hardware before submitting.

## License

Apache License 2.0. See [LICENSE](LICENSE).

```
Copyright 2026 ESP32-C5 Zigbee2ESPHome Contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

## Resources

**ESP32-C5 and Zigbee** —
[C5 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c5_datasheet_en.pdf) ·
[WROOM-1/1U datasheet](https://documentation.espressif.com/esp32-c5-wroom-1_wroom-1u_datasheet_en.html) ·
[ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/) ·
[ESP-Zigbee-SDK](https://github.com/espressif/esp-zigbee-sdk)

**ESPHome** —
[documentation](https://esphome.io/) ·
[native API](https://esphome.io/components/api.html) ·
[aioesphomeapi](https://github.com/esphome/aioesphomeapi)

**Home Assistant** —
[ESPHome integration](https://www.home-assistant.io/integrations/esphome/) ·
[Bluetooth integration](https://www.home-assistant.io/integrations/bluetooth/)

---

**Keywords**: ESP32-C5, Zigbee coordinator, ESPHome native API, Home Assistant,
Zigbee gateway without MQTT, zigbee2mqtt alternative, ESP32 Zigbee bridge,
dual-band Zigbee gateway, ESP-IDF, zigbee-herdsman-converters.
