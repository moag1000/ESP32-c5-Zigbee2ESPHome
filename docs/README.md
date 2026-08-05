# Dokumentation: was hier gilt und was nicht

**Kurz: `CLAUDE.md` im Projektwurzelverzeichnis ist die gepflegte Quelle.**
Die Dateien hier stammen groesstenteils vom Fork-Stand **2026-02-19** und
beschreiben teilweise Code, den es nicht mehr gibt.

Jede betroffene Datei traegt oben einen datierten Hinweis, der **konkret**
benennt, was an ihr veraltet ist — kein pauschales "koennte alt sein". Das ist
Absicht: ein Dokument, das selbstbewusst Falsches behauptet, kostet mehr Zeit
als eines, das fehlt. `cluster_state_ng` stand hier und in `CLAUDE.md` lange
als zentraler State-Mechanismus beschrieben, waehrend es **kein einziger
Aufruf** je erreichte — das hat mindestens eine Fehlersuche in die falsche
Richtung geschickt.

## Die drei grossen Aenderungen, die fast alles hier betreffen

1. **BLE ist projektweit abgeschaltet** (`CONFIG_BT_ENABLED=n`). Der Code liegt
   im Baum, wird aber nicht kompiliert. Der C5 haelt unter
   WiFi+Zigbee-Koexistenzlast keine stabilen GATT-Verbindungen. Jede
   BLE-Beschreibung hier ist "was der Code koennte", nicht "was laeuft".

2. **Converter kommen zur Laufzeit aus LittleFS**, nicht mehr einkompiliert.
   In C liegen noch drei; alles andere ist eine JSON-DB unter
   `/littlefs/converters`.

3. **Zigbee haengt nicht mehr am Uplink.** Der Koordinator startet vor der
   MQTT-Phase. Die aeltere Reihenfolge WiFi -> MQTT -> Zigbee ist die, bei der
   das Geraet ohne Access Point gar kein Zigbee mehr machte.

## Stand der einzelnen Dateien

| Datei | Stand |
|---|---|
| `ARCHITECTURE.md` | teilweise veraltet, Hinweis fortgeschrieben |
| `API_REFERENCE.md` | Verweise auf entferntes `zb_device_handler` |
| `ACTION_PLAN.md` | historisch — Plan ist ausgefuehrt |
| `HYBRID_ARCHITECTURE_PLAN.md` | historisch — Plan ist umgesetzt |
| `COEX_MANAGER_CONCEPT.md` | Konzept gueltig, Implementierung geloescht |
| `COEXISTENCE.md`, `MEMORY_OPTIMIZATION.md` | BLE-Zahlen ueberholt |
| `BLUETOOTH_GATEWAY.md`, `BLE_DEVICES.md` | beschreiben abgeschalteten Code |
| `ESPHOME_API.md` | Entity-Spiegel hat inzwischen eigene Ablage |
| `MQTT_PROTOCOL.md` | BLE-Topics inaktiv; MQTT ist sekundaer |
| `CODE_STYLE.md` | Stilregeln gelten, BLE-Beispiele nicht |
| `CONFIGURATION.md`, `INSTALLATION.md`, `INTEGRATION.md`, `USAGE.md`, `HARDWARE_SETUP.md`, `TROUBLESHOOTING.md`, `DEVELOPMENT.md`, `USER_STORIES.md` | einzelne BLE-Stellen inaktiv |
| `TUYA_GENERIC_DRIVER_PLAN.md` | aktuell |
| `ZIGBEE_SDK_MIGRATION.md` | aktuell |

## Wenn du hier etwas aenderst

Pruef die Behauptung gegen den Code, nicht gegen ein anderes Dokument. Fuer
"wird dieses Modul ueberhaupt benutzt?" ist die Linker-Map die verlaessliche
Quelle, nicht die Textsuche:

```bash
grep -oE "libmain\.a\([a-z_0-9]+\.c\.obj\)" build/esp32_c5_zigbee2mqtt.map \
  | sed 's/.*(\(.*\))/\1/' | sort -u
```

Eine Textsuche nach `<modul>_init` taeuscht in beide Richtungen: Aufrufe aus
ebenfalls totem Code zaehlen mit, und Substring-Treffer wie
`evt_zb_ota_progress_t` sehen aus wie Verwendung.
