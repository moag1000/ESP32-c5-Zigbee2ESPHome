# Third-party notices

This gateway stands on other people's work. Two projects in particular carry it:
the device knowledge that makes a Zigbee device more than an address, and the
protocol that lets Home Assistant talk to it. Neither is a dependency in the
usual sense — without them there would be no product here at all.

## The device database

### zigbee-herdsman-converters

**MIT License — Copyright (c) 2018 Koen Kanters**
<https://github.com/Koenkk/zigbee-herdsman-converters>

This is the reason a paired device shows up as "Fingerbot Plus" with a mode
selector and a sustain time rather than as an unknown address with a few
clusters. Thousands of devices, described by hundreds of contributors over
years, each entry usually the result of somebody buying a device and working
out what its vendor actually did.

- Sources vendored at `tools/zhc/` (version 26.91.0), with the upstream
  `LICENSE` preserved in place.
- `tools/z2m_converter_extract.py` reads those sources and emits the compact
  JSON under `data/converters/`.
- The result is merged into `data/converters_merged/` and shipped to the
  device's LittleFS partition.

Everything in `data/converters/` is derived from this project. When it moves,
the right thing to do is re-run the extraction rather than hand-edit the output.

### zha-device-handlers (ZHA quirks)

**Apache License 2.0**
<https://github.com/zigpy/zha-device-handlers>

Covers devices the z2m set does not, and disagrees with it usefully in places.

- Sources vendored at `tools/zhaquirks/`, upstream `LICENSE.md` preserved.
- `tools/zhaquirks_transpiler.py` produces `data/converters_zhaquirks/`.
- `tools/merge_converter_dbs.py` reconciles both into the shipped database.

## The protocol

### ESPHome

<https://github.com/esphome/esphome>

The native API this gateway speaks — framing, message type IDs, entity
descriptions, the Noise_NNpsk0 handshake — is ESPHome's design. The
implementation in `main/esphome/` is written for this project, but the wire
format and its numbering come from ESPHome's `api.proto`, and the entity model
Home Assistant expects is theirs.

Getting a message type ID wrong here put Home Assistant into a disconnect loop;
`tests/unit/test_esphome_protocol.c` pins those numbers by hand for that reason.

## Platform and components

- **ESP-IDF** — Apache License 2.0, Espressif Systems
- **esp-zigbee-lib**, **esp-zboss-lib** — Espressif; zboss additionally
  Copyright (c) 2012-2021 DSR Corporation
- **cJSON** — MIT, Copyright (c) 2009-2017 Dave Gamble and cJSON contributors
- **littlefs** (`joltwallet__littlefs`) — Copyright 2020 Brian Pugh; littlefs
  itself BSD-3-Clause, Copyright (c) 2022 The littlefs authors
- **espressif__mqtt**, **espressif__mdns**, **espressif__led_strip** —
  Apache License 2.0, Espressif Systems

## This project

Apache License 2.0 — see `LICENSE`.

The licences above are compatible with that, and each upstream's own licence
file is kept alongside its vendored sources. If you redistribute this, keep them
there.
