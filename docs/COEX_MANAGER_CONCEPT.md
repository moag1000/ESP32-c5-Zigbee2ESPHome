# Koexistenz-Manager Konzept: Zigbee / BLE / ESPHome / WiFi

> **Status**: Konzeptidee / Draft — noch nicht implementiert

## Motivation

Der ESP32-C5 hat ein **einziges 2.4GHz-Funkmodul** für WiFi, BLE und Zigbee. Der Hardware-Arbiter von Espressif vergibt Funkzeit nach Prioritäten:

```
Priorität (hoch → niedrig):
  WiFi TX/RX Mgmt Frames  >  Zigbee TX/ACK  >  BLE Connected  >  WiFi Data  >  BLE Scan  >  Zigbee RX (idle)
```

Zigbee RX im Idle hat die niedrigste Priorität — jede BLE- oder WiFi-Aktivität unterbricht den Zigbee-Empfang. Bei einem Zigbee-Coordinator, der ständig empfangsbereit sein muss, ist das kritisch. Gleichzeitig darf ESPHome BLE Proxy nicht gestört werden, wenn Home Assistant aktive BLE-Verbindungen hat.

## Ist-Zustand

| Parameter | Wert | Bewertung |
|-----------|------|-----------|
| BLE Scan Interval | 3000ms | Gut |
| BLE Scan Window | 100ms | Gut (3.3% Duty) |
| BLE Pause bei Permit Join | Ja (manuell in zb_coordinator.c) | Gut, aber isoliert |
| BLE Pause bei Interview | Ja (manuell in zb_interview.c) | Gut, aber isoliert |
| WiFi Band | 5GHz bevorzugt | Gut |
| WiFi Power Save | Aus (`WIFI_PS_NONE`) | Suboptimal |
| Zigbee Channel | 25 | Gut |
| Dynamische BLE-Anpassung | Keine | Fehlend |
| Zigbee-Traffic-Awareness | Keine | Fehlend |
| ESPHome-Proxy-Awareness | Keine | Fehlend |
| MQTT-Batching | Keines (außer mqtt_logger) | Fehlend |

## Konzept: Zentraler Koexistenz-Manager

### Architektur

```
                    ┌──────────────────────────────┐
                    │    coex_manager (zentral)     │
                    │                              │
                    │  State Machine:               │
                    │  ZB Activity + ESPHome State  │
                    │  → BLE Mode + WiFi PS         │
                    └──────┬───────┬───────────────┘
                           │       │
              ┌────────────┼───────┼────────────────┐
              ▼            ▼       ▼                ▼
      ┌──────────┐  ┌─────────┐ ┌───────────┐  ┌──────────┐
      │ Zigbee   │  │ ESPHome │ │ BLE       │  │ WiFi/    │
      │ Activity │  │ Proxy   │ │ Scanner   │  │ MQTT     │
      │ Monitor  │  │ Events  │ │ Scheduler │  │ Batcher  │
      └──────────┘  └─────────┘ └───────────┘  └──────────┘
```

### Neue Dateien

| Datei | Zweck |
|-------|-------|
| `main/core/coex_manager.h` | Public API, Typen, Konfiguration |
| `main/core/coex_manager.c` | State Machine, Timer, BLE/MQTT-Steuerung |

### Zu modifizierende Dateien

| Datei | Änderung |
|-------|----------|
| `main/main.c` | `coex_manager_init()` nach Zigbee-Start, vor BLE-Init |
| `main/CMakeLists.txt` | `coex_manager.c` zu SRCS hinzufügen |
| `main/zigbee/zb_callbacks.c` | Activity-Notifications an coex_manager |
| `main/zigbee/zb_interview.c` | Interview Start/Ende an coex_manager |
| `main/zigbee/zb_ota.c` | OTA Transfer Start/Ende an coex_manager |
| `main/zigbee/zb_coordinator.c` | Permit-Join-Logik durch coex_manager ersetzen |
| `main/bluetooth/ble_scanner.h/c` | `ble_scanner_set_window()` hinzufügen |
| `main/esphome/esphome_ble_proxy.c` | GATT-Connect/Disconnect + ADV-Subscribe an coex_manager |
| `main/wifi/wifi_manager.c/h` | `wifi_manager_set_power_save()`, `wifi_manager_is_5ghz_connected()` |
| `main/mqtt/gateway_mqtt.c/h` | MQTT Batch-Queue + Flush-Timer |
| `main/core/device_state_publisher.c` | Umstellung auf batched publish |
| `main/core/monitoring/system_monitor.c` | Umstellung auf batched publish |
| `main/Kconfig.projbuild` | Coex-Konfigurationsmenü |

---

## Entscheidungsmatrix: BLE-Modus

Die Kernlogik bestimmt den BLE-Modus basierend auf Zigbee- und ESPHome-Aktivität:

```
                    │ ESPHome IDLE │ ESPHome CONNECTED │ ESPHome ADV_SUB │ ESPHome GATT │
────────────────────┼──────────────┼───────────────────┼─────────────────┼──────────────┤
ZB IDLE             │ BLE_ELEVATED │ BLE_ELEVATED      │ BLE_ELEVATED    │ BLE_GATT     │
ZB LOW_TRAFFIC      │ BLE_NORMAL   │ BLE_NORMAL        │ BLE_NORMAL      │ BLE_GATT     │
ZB DATA_EXCHANGE    │ BLE_MINIMAL  │ BLE_MINIMAL       │ BLE_NORMAL *    │ BLE_GATT *   │
ZB INTERVIEW        │ BLE_OFF      │ BLE_OFF           │ BLE_MINIMAL *   │ BLE_GATT *   │
ZB OTA_TRANSFER     │ BLE_OFF      │ BLE_OFF           │ BLE_MINIMAL *   │ BLE_GATT *   │
ZB DEVICE_JOINING   │ BLE_OFF      │ BLE_OFF           │ BLE_OFF         │ BLE_OFF      │
```

### BLE-Modi

| Modus | Interval | Window | Duty Cycle | Verwendung |
|-------|----------|--------|------------|------------|
| `BLE_OFF` | — | — | 0% | BLE komplett pausiert |
| `BLE_MINIMAL` | 5000ms | 50ms | 1% | Zigbee-Last hoch, aber ESPHome braucht BLE |
| `BLE_NORMAL` | 3000ms | 100ms | 3.3% | Standard-Betrieb (aktueller Default) |
| `BLE_ELEVATED` | 2000ms | 150ms | 7.5% | Zigbee idle, mehr BLE-Zeit verfügbar |
| `BLE_GATT` | — | — | — | Scanning aus, GATT-Connection aktiv |

### Schlüsselregeln

1. **ESPHome ADV_SUBSCRIBED** → BLE nie komplett aus (außer Permit Join)
2. **ESPHome GATT_ACTIVE** → Scanning pausiert, GATT-Connection hat Vorrang
3. **ZB DEVICE_JOINING** → BLE immer aus (Zigbee-Joining ist zeitkritisch)
4. **ZB IDLE** → Mehr BLE-Zeit, da Zigbee wenig Funkzeit braucht

---

## Zigbee-Activity-Tracking

### Aktivitätsstufen

```c
typedef enum {
    COEX_ZB_IDLE,            /* Kein Traffic seit >5s */
    COEX_ZB_LOW_TRAFFIC,     /* Gelegentliche Reports */
    COEX_ZB_DATA_EXCHANGE,   /* Aktiver Datenaustausch */
    COEX_ZB_INTERVIEW,       /* Device Interview läuft */
    COEX_ZB_OTA_TRANSFER,    /* OTA Update aktiv */
    COEX_ZB_DEVICE_JOINING,  /* Permit Join aktiv */
} coex_zb_activity_t;
```

### Hook-Points

| Quelle | Callback | Aktivität |
|--------|----------|-----------|
| `zb_callbacks.c` | `ESP_ZB_CORE_REPORT_ATTR_CB_ID` | `DATA_EXCHANGE` |
| `zb_callbacks.c` | `ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID` | `DATA_EXCHANGE` |
| `zb_callbacks.c` | `ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID` | `DATA_EXCHANGE` |
| `zb_interview.c` | `zb_interview_start()` | `INTERVIEW` |
| `zb_interview.c` | `interview_complete()` | → `IDLE` |
| `zb_ota.c` | `create_transfer()` | `OTA_TRANSFER` |
| `zb_ota.c` | Transfer completion | → `IDLE` |
| `zb_coordinator.c` | Permit Join Start | `DEVICE_JOINING` |
| `zb_coordinator.c` | Permit Join Ende | → `IDLE` |

### Automatische Degradierung (500ms Timer-Tick)

- `DATA_EXCHANGE` → `IDLE` nach 5s ohne Traffic
- `LOW_TRAFFIC` → `IDLE` nach 10s ohne Traffic
- `INTERVIEW`/`OTA`/`JOINING` bleiben bis explizites Ende

---

## ESPHome-Activity-Tracking

### Aktivitätsstufen

```c
typedef enum {
    COEX_ESPHOME_IDLE,           /* Keine Clients verbunden */
    COEX_ESPHOME_CONNECTED,      /* Client(s) verbunden, kein BLE-Proxy */
    COEX_ESPHOME_ADV_SUBSCRIBED, /* Client abonniert BLE-Advertisements */
    COEX_ESPHOME_GATT_ACTIVE,    /* GATT-Proxy-Operation läuft */
} coex_esphome_activity_t;
```

### Hook-Points

| Quelle | Event | Aktivität |
|--------|-------|-----------|
| `esphome_ble_proxy.c` | Client subscribes to ADV | `ADV_SUBSCRIBED` |
| `esphome_ble_proxy.c` | `on_gatt_connect()` | `GATT_ACTIVE` |
| `esphome_ble_proxy.c` | `on_gatt_disconnect()` | → `ADV_SUBSCRIBED` oder `IDLE` |
| `esphome_api_server.c` | Client connect | `CONNECTED` |
| `esphome_api_server.c` | Client disconnect | → Prüfung ob andere Clients aktiv |

### Warum ESPHome-Awareness wichtig ist

- **BLE Proxy ADV Forwarding**: Wenn HA BLE-Advertisements empfangen will, muss der Scanner laufen — auch bei Zigbee-Last
- **GATT Proxy**: Wenn HA eine BLE-Gerät-Verbindung über den Proxy hat (z.B. Fingerbot, SwitchBot), darf die GATT-Connection nicht durch Scanner-Restart unterbrochen werden
- **ADV Queue** (32 Items × ~83B): Bei zu langen Scan-Pausen läuft die Queue leer und HA bekommt keine BLE-Geräte-Updates

---

## MQTT Batching

### Motivation

Jeder einzelne `mqtt_client_publish()` Aufruf erzeugt WiFi-Aktivität, die den Radio-Arbiter beschäftigt und Zigbee/BLE unterbricht. Durch Bündelung in Bursts wird die WiFi-Funkzeit konzentriert.

### Design

```c
/* Sofort senden (nicht batchen): */
- QoS >= 1
- retain == true
- Bridge Events (zigbee2mqtt/bridge/event)
- Bridge Responses (zigbee2mqtt/bridge/response/*)
- HA Discovery (homeassistant/**/config)
- Bridge State/Info/Devices

/* Batchen (QoS 0, retain false): */
- Device State Updates (zigbee2mqtt/{device})
- System Monitor Stats
- Diagnostics
```

### Batch-Parameter

| Parameter | Default | Kconfig |
|-----------|---------|---------|
| Flush-Intervall | 1000ms | `CONFIG_COEX_MQTT_BATCH_INTERVAL_MS` |
| Max Queue-Größe | 16 | Hardcoded |
| Payload-Speicher | PSRAM | `heap_caps_malloc(MALLOC_CAP_SPIRAM)` |

### Flush-Trigger

1. **Timer**: Alle `BATCH_INTERVAL_MS`
2. **Queue voll**: Sofortiger Flush bei 16 gepufferten Nachrichten
3. **Explizit**: `mqtt_client_batch_flush()` bei kritischen Zustandsänderungen

---

## WiFi Power Save (Adaptive)

### Logik

```
5GHz + kein MQTT pending + Zigbee aktiv → WIFI_PS_MIN_MODEM (Light Sleep)
Sonst                                   → WIFI_PS_NONE (immer wach)
```

### Warum nur bei 5GHz

Bei 2.4GHz konkurriert WiFi direkt mit Zigbee/BLE um das Funkmodul. Power Save könnte dazu führen, dass WiFi-Management-Frames (SA Query, Deauth) verpasst werden. Bei 5GHz gibt es keine Frequenzkollision, daher kann WiFi zwischen DTIM-Beacons schlafen und gibt dem Funkmodul mehr Zeit für Zigbee/BLE.

### Erwartete Einsparung

`WIFI_PS_MIN_MODEM` mit Listen-Interval 1: WiFi wacht alle ~100ms für Beacon auf, den Rest der Zeit hat Zigbee/BLE freien Funkzugang.

---

## Konsolidierung bestehender BLE-Pause-Logik

Aktuell existieren **drei unabhängige** BLE-Pause-Mechanismen:

| Ort | Mechanismus | Wird ersetzt durch |
|-----|-------------|-------------------|
| `zb_coordinator.c` | `s_ble_paused_for_join` + manuelles stop/start | `coex_manager_notify_zb_activity(DEVICE_JOINING)` |
| `zb_interview.c` | `s_ble_paused_for_interview` + manuelles stop/start | `coex_manager_notify_zb_activity(INTERVIEW)` |
| `ble_manager.c` | `ble_manager_set_coexist_mode()` Interval-Doubling | Beibehalten als Fallback, coex_manager hat Vorrang |

---

## Zeitliche Koordination — Gesamtbild

```
1 Sekunde Zeitachse (5GHz WiFi, normaler Betrieb):
┌────────────────────────────────────────────────────────────────────┐
│ WiFi    ║░░║        ║░░║         ║░░║         ║████████║░░║       │
│ beacon  ║  ║        ║  ║         ║  ║         ║MQTT    ║  ║       │
│         ║  ║        ║  ║         ║  ║         ║Burst   ║  ║       │
│─────────║──║────────║──║─────────║──║─────────║────────║──║───────│
│ BLE     ║  ║        ║  ║ ████   ║  ║         ║        ║  ║       │
│ scan    ║  ║        ║  ║ 100ms  ║  ║         ║        ║  ║       │
│─────────║──║────────║──║────────║──║─────────║────────║──║───────│
│ Zigbee  ║██████████████║████████║██████████████║      ║████████████│
│ RX/TX   ║  aktiv        ║aktiv  ║  aktiv       ║pause ║  aktiv   │
└────────────────────────────────────────────────────────────────────┘
  ░░ = WiFi Beacon RX (~5ms)    ████ = aktiv    leer = sleep/idle
```

---

## Kconfig-Optionen

```
menu "Coexistence Manager"
    config COEX_MANAGER_ENABLED
        bool "Enable Coexistence Manager"
        default y

    config COEX_MQTT_BATCHING
        bool "Enable MQTT publish batching"
        default y
        depends on COEX_MANAGER_ENABLED

    config COEX_MQTT_BATCH_INTERVAL_MS
        int "MQTT batch flush interval (ms)"
        default 1000
        range 100 5000
        depends on COEX_MQTT_BATCHING

    config COEX_WIFI_ADAPTIVE_PS
        bool "Adaptive WiFi power save (5GHz only)"
        default y
        depends on COEX_MANAGER_ENABLED

    config COEX_BLE_ADAPTIVE_DUTY
        bool "Adaptive BLE scan duty cycle"
        default y
        depends on COEX_MANAGER_ENABLED
endmenu
```

---

## Erwartete Verbesserungen

| Metrik | Vorher | Nachher (geschätzt) |
|--------|--------|---------------------|
| Zigbee Paketvertust | ~2% | ~0.5-1% |
| BLE Scan Coverage (Idle) | 3.3% Duty | 7.5% Duty |
| BLE Scan Coverage (ZB aktiv) | 3.3% Duty | 1% Duty (bewusst reduziert) |
| WiFi-Funkunterbrechungen | Kontinuierlich | Gebündelt (MQTT Batching) |
| ESPHome BLE Proxy Unterbrechung | Bei ZB Interview/Join | Nur bei Permit Join |
| BLE-Pause-Mechanismen | 3 unabhängige | 1 zentraler Manager |

---

## Referenzen

- [ESP32-C5 RF Coexistence (ESP-IDF v5.5.1)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/coexist.html)
- [ESP-IDF esp_coexist.h API](https://github.com/espressif/esp-idf/blob/master/components/esp_coex/include/esp_coexist.h)
- [docs/COEXISTENCE.md](COEXISTENCE.md) — Bestehende Koexistenz-Dokumentation
