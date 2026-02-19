# User Stories - ESP32-C5 Unified Gateway

## Personas

| Persona | Beschreibung | Technisches Level |
|---------|-------------|-------------------|
| **Anna** | Smart-Home-Einsteigerin, hat Home Assistant auf einem Pi laufen, erste Zigbee-Geräte | Anfänger |
| **Marco** | Home-Automation-Enthusiast, 30+ Geräte, nutzt MQTT und Automationen aktiv | Fortgeschritten |
| **Lisa** | Entwicklerin, will das Gateway erweitern, eigene Converter schreiben | Experte |

---

## Phase 1: Inbetriebnahme & Ersteinrichtung

### US-1.1: Firmware flashen
> **Als** Anna **möchte ich** die Firmware auf mein ESP32-C5-DevKit flashen können,
> **damit** ich das Gateway in Betrieb nehmen kann.

**Akzeptanzkriterien:**
- [x] Dokumentierte Anleitung für Firmware-Download und Flash-Vorgang
- [x] `scripts/flash.sh` erkennt den seriellen Port automatisch (oder gibt klare Fehlermeldung)
- [x] LED blinkt nach erfolgreichem Flash in einem definierten Muster ("Bereit zur Einrichtung")
- [x] Bei fehlgeschlagenem Flash bleibt das Gerät im Bootloader (kein Brick)

**Abhängigkeiten:** Keine
**Komponenten:** `main.c`, `led/`, `scripts/flash.sh`

---

### US-1.2: WiFi-Verbindung über Captive Portal herstellen
> **Als** Anna **möchte ich** mein Gateway per Smartphone mit dem WiFi verbinden,
> **damit** ich keine serielle Konsole brauche.

**Akzeptanzkriterien:**
- [x] Gateway öffnet einen Access Point (z.B. `ESP32-Gateway-XXXX`) wenn kein WiFi konfiguriert ist
- [x] Captive Portal erscheint automatisch beim Verbinden mit dem AP
- [x] Portal zeigt verfügbare WiFi-Netzwerke mit Signalstärke
- [x] 5GHz-Netzwerke werden bevorzugt angezeigt (falls unterstützt)
- [x] Nach Eingabe der Credentials verbindet sich das Gateway und zeigt Erfolg/Fehler
- [x] LED wechselt auf "WiFi verbunden"-Muster
- [x] Bei falschem Passwort: Rückkehr zum Portal mit Fehlermeldung
- [x] Credentials werden persistent in NVS gespeichert

**Abhängigkeiten:** US-1.1
**Komponenten:** `wifi/`, `wifi/captive_portal.c`, `led/`, NVS

---

### US-1.3: MQTT-Broker konfigurieren
> **Als** Anna **möchte ich** meinen MQTT-Broker (Mosquitto auf dem Pi) im Gateway eintragen,
> **damit** das Gateway mit Home Assistant kommunizieren kann.

**Akzeptanzkriterien:**
- [x] MQTT-Konfiguration ist über das Captive Portal oder eine Web-UI möglich
- [x] Felder: Broker-Adresse, Port, Benutzername, Passwort (optional), Base-Topic
- [x] Verbindungstest-Button ("Teste Verbindung") mit sofortigem Feedback
- [x] TLS/SSL Option (Port 8883) für sichere Verbindung
- [x] Default-Base-Topic: `zigbee2mqtt` (kompatibel mit Z2M)
- [x] Status-Topic `zigbee2mqtt/bridge/state` wird nach Verbindung publiziert
- [x] Bei Verbindungsverlust: automatische Reconnection mit Backoff

**Abhängigkeiten:** US-1.2
**Komponenten:** `mqtt/`, `core/bridge/mqtt_bridge.c`, NVS

---

### US-1.4: Gerätestatus nach Ersteinrichtung prüfen
> **Als** Anna **möchte ich** auf einen Blick sehen, ob alles korrekt eingerichtet ist,
> **damit** ich weiß, dass das Gateway bereit ist.

**Akzeptanzkriterien:**
- [x] LED zeigt dauerhaft "Betriebsbereit" (z.B. langsames grünes Pulsieren)
- [x] MQTT-Topic `zigbee2mqtt/bridge/info` enthält: Version, Koordinator-Status, Uptime
- [x] MQTT-Topic `zigbee2mqtt/bridge/state` zeigt `online`
- [x] Home Assistant erkennt das Gateway automatisch (MQTT Discovery)
- [x] Freier Heap und Speicherstatus abrufbar via MQTT

**Abhängigkeiten:** US-1.3
**Komponenten:** `core/bridge/`, `core/monitoring/`, `led/`, `core/discovery/ha_discovery.c`

---

## Phase 2: Zigbee-Geräte verwalten

### US-2.1: Zigbee-Netzwerk starten
> **Als** Anna **möchte ich**, dass das Gateway automatisch ein Zigbee-Netzwerk aufbaut,
> **damit** ich sofort Geräte pairen kann.

**Akzeptanzkriterien:**
- [x] Zigbee-Koordinator startet automatisch nach WiFi+MQTT-Verbindung
- [x] PAN-ID und Kanal werden automatisch gewählt (oder konfigurierbar)
- [x] Network Key wird sicher generiert und in NVS gespeichert
- [x] Status via MQTT: `zigbee2mqtt/bridge/info` enthält `coordinator` Daten (Kanal, PAN-ID, IEEE-Adresse)
- [x] Bei Neustart: bestehendes Netzwerk wird wiederhergestellt (kein Re-Pairing nötig)

**Abhängigkeiten:** US-1.3
**Komponenten:** `zigbee/zb_coordinator.c`, `zb_storage`, NVS

---

### US-2.2: Zigbee-Gerät pairen (Permit Join)
> **Als** Anna **möchte ich** ein neues Zigbee-Gerät (z.B. IKEA Lampe) mit dem Gateway verbinden,
> **damit** es in Home Assistant erscheint.

**Akzeptanzkriterien:**
- [x] Pairing-Modus aktivierbar via MQTT: `zigbee2mqtt/bridge/request/permit_join` → `{"value": true, "time": 120}`
- [x] Pairing-Modus aktivierbar via Home Assistant Button-Entity
- [x] LED zeigt "Pairing-Modus aktiv" (z.B. schnelles Blinken)
- [x] Timeout nach konfigurierbarer Zeit (default: 120s)
- [x] Status-Feedback: `zigbee2mqtt/bridge/response/permit_join` bestätigt Aktivierung
- [x] Neues Gerät wird erkannt und Interview-Prozess startet automatisch
- [x] Fortschritt via MQTT: `zigbee2mqtt/bridge/event` → `{"type": "device_joined", ...}`

**Abhängigkeiten:** US-2.1
**Komponenten:** `zigbee/zb_coordinator.c`, `zigbee/zb_callbacks.c`, `led/`, Event Bus

---

### US-2.3: Geräte-Interview und Erkennung
> **Als** Anna **möchte ich**, dass mein neues Gerät automatisch erkannt wird und der richtige Gerätetyp zugewiesen wird,
> **damit** ich es sofort in Home Assistant steuern kann.

**Akzeptanzkriterien:**
- [x] Interview-Prozess läuft automatisch nach Join (Model, Manufacturer, Endpoints, Clusters)
- [x] Bekannte Geräte (IKEA, Philips, Xiaomi, Sonoff, Tuya, Lidl) werden automatisch erkannt
- [x] Passender Converter wird zugewiesen → korrekte Entities in HA
- [x] Unbekannte Geräte bekommen generische Entities basierend auf ihren Clustern
- [x] Interview-Status via MQTT: `zigbee2mqtt/bridge/event` → `{"type": "device_interview", "status": "successful"}`
- [x] Gerät erscheint in HA Discovery innerhalb von ~10 Sekunden
- [x] Friendly Name wird vergeben (z.B. `0x00158d0001234567`)

**Abhängigkeiten:** US-2.2
**Komponenten:** `zigbee/zb_interview.c`, `zigbee/converter/`, `core/discovery/ha_discovery_ng.c`, `core/device/device_registry.c`

---

### US-2.4: Zigbee-Gerät steuern
> **Als** Anna **möchte ich** meine IKEA-Lampe über Home Assistant ein-/ausschalten und dimmen,
> **damit** ich sie in Automationen verwenden kann.

**Akzeptanzkriterien:**
- [x] Befehle via MQTT: `zigbee2mqtt/<friendly_name>/set` → `{"state": "ON", "brightness": 200}`
- [x] Befehle via Home Assistant UI (Schalter, Dimmer, Farbrad)
- [x] Antwort/State-Update via MQTT: `zigbee2mqtt/<friendly_name>` → `{"state": "ON", "brightness": 200}`
- [x] Latenz: Befehl → Ausführung < 500ms (typisch < 200ms)
- [x] Fehler-Feedback bei nicht erreichbarem Gerät (availability)

**Abhängigkeiten:** US-2.3
**Komponenten:** `core/adapters/mqtt_adapter.c`, `zigbee/zb_callbacks.c`, ZCL Cluster Handler

---

### US-2.5: Sensordaten empfangen
> **Als** Marco **möchte ich** Temperatur, Luftfeuchtigkeit und Batteriestatus meiner Zigbee-Sensoren sehen,
> **damit** ich Automationen basierend auf Raumklima erstellen kann.

**Akzeptanzkriterien:**
- [x] Periodische Reports werden empfangen und via MQTT publiziert
- [x] Format: `zigbee2mqtt/<friendly_name>` → `{"temperature": 21.5, "humidity": 55, "battery": 87}`
- [x] Einheiten sind korrekt (°C, %, Lux, hPa)
- [x] HA-Entities haben korrekte `device_class` und `unit_of_measurement`
- [x] Batterie-Warnung bei < 20% (optional konfigurierbar)
- [x] Reporting-Intervall konfigurierbar via MQTT

**Abhängigkeiten:** US-2.3
**Komponenten:** `zigbee/zb_reporting.c`, `zigbee/cluster_state_ng.c`, `core/adapters/mqtt_adapter.c`

---

### US-2.6: Geräteliste abrufen
> **Als** Marco **möchte ich** eine Übersicht aller verbundenen Zigbee-Geräte sehen,
> **damit** ich den Überblick über mein Netzwerk behalte.

**Akzeptanzkriterien:**
- [x] Geräteliste via MQTT: `zigbee2mqtt/bridge/request/devices` → Antwort mit Liste
- [x] Pro Gerät: IEEE-Adresse, Friendly Name, Modell, Hersteller, Typ, letzter Kontakt, Batterielevel
- [x] Verfügbarkeitsstatus (online/offline) pro Gerät
- [x] Netzwerk-Topologie abrufbar (welches Gerät routet über welches)
- [x] Link-Quality (LQI) pro Verbindung

**Abhängigkeiten:** US-2.3
**Komponenten:** `core/device/device_registry.c`, `core/bridge/bridge_request_handler.c`

---

### US-2.7: Gerät umbenennen
> **Als** Anna **möchte ich** meinen Geräten sprechende Namen geben (z.B. "Wohnzimmer Lampe"),
> **damit** ich sie in Home Assistant leichter finde.

**Akzeptanzkriterien:**
- [x] Umbenennen via MQTT: `zigbee2mqtt/bridge/request/device/rename` → `{"from": "0x00158d...", "to": "Wohnzimmer Lampe"}`
- [x] MQTT-Topics ändern sich automatisch auf neuen Namen
- [x] HA-Entities werden mit neuem Namen aktualisiert
- [x] Name wird persistent gespeichert
- [x] Sonderzeichen und Umlaute werden korrekt behandelt

**Abhängigkeiten:** US-2.3
**Komponenten:** `core/bridge/bridge_request_handler.c`, `core/device/device_persistence.c`

---

### US-2.8: Gerät entfernen
> **Als** Marco **möchte ich** ein defektes oder nicht mehr benötigtes Gerät aus dem Netzwerk entfernen,
> **damit** es keine Ressourcen mehr belegt.

**Akzeptanzkriterien:**
- [x] Entfernen via MQTT: `zigbee2mqtt/bridge/request/device/remove` → `{"id": "0x00158d..."}`
- [x] Force-Remove Option für nicht erreichbare Geräte: `{"id": "...", "force": true}`
- [x] Gerät wird aus Registry gelöscht
- [x] HA-Entities werden entfernt (Discovery: leere Config)
- [x] MQTT-Topics werden bereinigt (retained Messages entfernt)
- [x] Bestätigung via MQTT Response

**Abhängigkeiten:** US-2.3
**Komponenten:** `zigbee/zb_coordinator.c`, `core/device/device_registry.c`, `core/discovery/ha_discovery_ng.c`

---

### US-2.9: Zigbee-Gruppen verwalten
> **Als** Marco **möchte ich** mehrere Lampen zu einer Gruppe zusammenfassen,
> **damit** ich sie gleichzeitig steuern kann.

**Akzeptanzkriterien:**
- [x] Gruppe erstellen via MQTT: `zigbee2mqtt/bridge/request/group/add` → `{"friendly_name": "Wohnzimmer"}`
- [x] Gerät zu Gruppe hinzufügen: `zigbee2mqtt/bridge/request/group/members/add`
- [x] Gruppenbefehl: `zigbee2mqtt/Wohnzimmer/set` → `{"state": "ON"}` schaltet alle Geräte
- [x] Multicast (Zigbee Group Command) für minimale Latenz
- [x] Gruppe erscheint als eigene Entity in HA

**Abhängigkeiten:** US-2.3
**Komponenten:** `zigbee/zb_groups.c`, `zigbee/zb_binding.c`

---

### US-2.10: Zigbee-OTA-Update für Endgeräte
> **Als** Marco **möchte ich** Firmware-Updates für meine Zigbee-Geräte über das Gateway einspielen können,
> **damit** sie aktuell und sicher bleiben.

**Akzeptanzkriterien:**
- [x] OTA-Image-Upload via MQTT oder HTTP
- [x] Verfügbare Updates anzeigen pro Gerät
- [x] Update-Prozess starten via MQTT-Befehl
- [x] Fortschrittsanzeige (%) via MQTT Events
- [x] Abbruch-Möglichkeit bei laufendem Update
- [x] Rollback-Info nach Update

**Abhängigkeiten:** US-2.3
**Komponenten:** `zigbee/zb_ota.c`, `ota/`

---

## Phase 3: Bluetooth-Geräte einbinden

> **Architektur-Hinweis:** BLE-Geräte werden NICHT über MQTT veröffentlicht,
> sondern ausschließlich über die **ESPHome Native API**. Der Datenfluss ist:
>
> `BLE Scanner → BLE Adapter → Event Bus → ESPHome Adapter → ESPHome Native API → Home Assistant`
>
> Zigbee-Geräte hingegen nutzen MQTT (Zigbee2MQTT-kompatibel) UND optional ESPHome.

### US-3.1: BLE-Scanning aktivieren
> **Als** Marco **möchte ich** BLE-Geräte in meiner Umgebung automatisch erkennen,
> **damit** ich z.B. Xiaomi-Temperatursensoren einbinden kann.

**Akzeptanzkriterien:**
- [x] BLE-Scanner ist aktivierbar/deaktivierbar via `CONFIG_BT_SCANNER_ENABLED` (menuconfig)
- [x] Erkannte Geräte erscheinen als ESPHome-Entities in Home Assistant (via Native API)
- [x] Filterung nach bekannten Gerätetypen (kein Spam mit unbekannten BLE-Geräten)
- [x] Scan-Intervall konfigurierbar (Default: alle 10s)
- [x] Koexistenz mit Zigbee: BLE-Scan unterbricht keine Zigbee-Kommunikation
- [x] Speicherverbrauch bleibt unter PSRAM-Budget (~40KB für BLE-Stack)

**Abhängigkeiten:** US-1.2 (WiFi für ESPHome), US-4.2 (ESPHome API), WiFi/BT/Zigbee-Koexistenz
**Komponenten:** `bluetooth/ble_manager.c`, `bluetooth/ble_scanner.c`, `core/adapters/ble_adapter.c`, `core/adapters/esphome_adapter.c`

---

### US-3.2: BLE-Temperatursensor einbinden (Xiaomi LYWSD03MMC)
> **Als** Marco **möchte ich** meinen Xiaomi-Temperatursensor ohne zusätzliche Hardware in Home Assistant sehen,
> **damit** ich nur ein Gateway für alles brauche.

**Akzeptanzkriterien:**
- [x] Xiaomi LYWSD03MMC wird automatisch erkannt (ATC/pvvx Custom Firmware)
- [x] Temperatur, Luftfeuchtigkeit, Batterie werden ausgelesen
- [x] Daten erscheinen als ESPHome-Sensor-Entities in Home Assistant (via Native API, nicht MQTT)
- [x] Passives Advertisement-Parsing (ATC-Format Big-Endian + pvvx-Format Little-Endian)
- [x] Scan-Intervall konfigurierbar
- [x] Validierung der Werte (-40°C bis 60°C, 0-100% Humidity)

**Abhängigkeiten:** US-3.1, US-4.2 (ESPHome API)
**Komponenten:** `bluetooth/devices/ble_xiaomi.c`, `core/adapters/ble_adapter.c`, `core/adapters/esphome_adapter.c`

---

### US-3.3: BLE-Präsenzerkennung
> **Als** Marco **möchte ich** anhand von BLE-Beacons (iBeacon, Smartphone) erkennen, ob Personen im Raum sind,
> **damit** ich Automationen für Anwesenheit erstellen kann.

**Akzeptanzkriterien:**
- [x] iBeacon und Eddystone-Beacons (UID + TLM) werden erkannt
- [x] RSSI wird in Distanz-Schätzung umgerechnet (Log-Distance Path Loss Model)
- [x] Presence-Levels: IMMEDIATE (< -50dBm), NEAR (< -60dBm), AWAY (< -75dBm)
- [x] Device-Tracker-Entity in HA via ESPHome Native API (nicht MQTT)
- [x] Konfigurierbarer Timeout für "Person abwesend" (Default: 5 Minuten)
- [x] Mehrere Beacons pro Person unterstützt

**Abhängigkeiten:** US-3.1, US-4.2 (ESPHome API)
**Komponenten:** `bluetooth/devices/ble_beacon.c`, `core/adapters/ble_adapter.c`, `core/adapters/esphome_adapter.c`

---

### US-3.4: BLE-Gerät manuell konfigurieren
> **Als** Marco **möchte ich** Bindkey und andere Parameter für verschlüsselte BLE-Sensoren eingeben können,
> **damit** auch geschützte Geräte ausgelesen werden können.

**Akzeptanzkriterien:**
- [x] Bindkey-Eingabe via ESPHome Service-Call oder Captive Portal
- [x] Unterstützte Verschlüsselungsarten: Xiaomi MiBeacon v2/v3/v4/v5
- [x] Key wird sicher in NVS gespeichert
- [x] Fehlermeldung bei falschem Key
- [x] Liste konfigurierter BLE-Geräte abrufbar via ESPHome-Entity oder MQTT-Bridge-Request

**Abhängigkeiten:** US-3.1, US-4.2 (ESPHome API)
**Komponenten:** `bluetooth/devices/`, `bluetooth/ble_security.c`, NVS, `esphome/esphome_services.c`

---

## Phase 4: Home Assistant Integration

### US-4.1: Automatische HA-Discovery (zwei Wege)
> **Als** Anna **möchte ich**, dass alle Geräte automatisch in Home Assistant erscheinen,
> **damit** ich nichts manuell konfigurieren muss.

**Akzeptanzkriterien:**
- [x] **Zigbee-Geräte** publizieren MQTT Discovery Config (`homeassistant/...` Topics)
- [x] **BLE-Geräte** erscheinen via ESPHome Native API (mDNS-Discovery, kein MQTT)
- [x] **Gateway-Diagnose** erscheint via ESPHome (Heap, Uptime, WiFi RSSI)
- [x] Entities haben korrekte `device_class`, `state_class`, `unit_of_measurement`
- [x] Geräte sind in HA nach Hersteller und Modell gruppiert
- [x] Firmware-Version des Gateways ist in HA sichtbar
- [x] Bei Gateway-Neustart: MQTT Discovery + ESPHome Entities werden erneut registriert

**Abhängigkeiten:** US-1.3 (MQTT für Zigbee), US-4.2 (ESPHome für BLE + Diagnose)
**Komponenten:** `core/discovery/ha_discovery_ng.c` (Zigbee→MQTT), `core/adapters/esphome_adapter.c` (BLE→ESPHome)

---

### US-4.2: ESPHome-Native-API-Integration (primär für BLE + Diagnose)
> **Als** Marco **möchte ich** das Gateway via ESPHome Native API in Home Assistant einbinden,
> **damit** BLE-Geräte und Gateway-Diagnose direkt ohne MQTT-Broker verfügbar sind.

**Akzeptanzkriterien:**
- [x] ESPHome API Server läuft auf Port 6053
- [x] Gateway wird per mDNS automatisch in HA erkannt
- [x] **BLE-Geräte** werden als ESPHome-Entities registriert (Temperatur, Humidity, Battery, Presence)
- [x] **Gateway-Diagnose** als ESPHome-Entities (Free Heap, WiFi RSSI, Uptime, CPU Usage)
- [x] Noise-Protocol-Encryption (NNpsk0) mit Pre-Shared Key
- [x] Entities werden über Protobuf-Protokoll synchronisiert, Echtzeit-Push
- [x] Koexistenz mit MQTT: Zigbee via MQTT, BLE via ESPHome, gleichzeitig nutzbar
- [x] BLE-Proxy-Funktionalität für ESPHome BLE-Geräte

**Abhängigkeiten:** US-1.2 (WiFi)
**Komponenten:** `esphome/esphome_api_server.c`, `esphome/esphome_api.c`, `esphome/esphome_ble_proxy.c`, `core/adapters/esphome_adapter.c`

---

### US-4.3: Gateway als HA-Gerät mit Diagnose-Entities
> **Als** Marco **möchte ich** den Zustand des Gateways selbst in HA überwachen,
> **damit** ich Probleme frühzeitig erkenne.

**Akzeptanzkriterien:**
- [x] Sensor-Entities: Freier Heap, PSRAM-Nutzung, Uptime, WiFi-RSSI, CPU-Temperatur
- [x] Sensor-Entity: Anzahl verbundener Zigbee-Geräte, BLE-Geräte
- [x] Binary Sensor: Online/Offline-Status
- [x] Diagnose-Entities: Zigbee-Kanal, PAN-ID, Firmware-Version, ESP-IDF-Version
- [x] Button-Entity: Neustart, Permit Join, Netzwerk-Scan
- [x] Alle Entities unter einem HA-Gerät "ESP32-C5 Gateway" gruppiert

**Abhängigkeiten:** US-4.2 (ESPHome API)
**Komponenten:** `esphome/esphome_api.c` (Entity-Keys), `core/monitoring/`, `core/discovery/ha_bridge_discovery.c`

---

## Phase 5: Täglicher Betrieb

### US-5.1: Automatischer Betrieb ohne Eingriff
> **Als** Anna **möchte ich**, dass das Gateway nach einem Stromausfall automatisch wieder startet und alle Geräte wieder verbunden sind,
> **damit** ich mich nicht darum kümmern muss.

**Akzeptanzkriterien:**
- [x] WiFi-Reconnect automatisch (mit Backoff)
- [x] MQTT-Reconnect automatisch (mit Backoff)
- [x] Zigbee-Netzwerk wird aus NVS/Flash wiederhergestellt
- [x] Alle Geräte re-joinen automatisch (Zigbee Rejoin)
- [x] BLE-Scanner startet automatisch
- [x] HA-Discovery wird nach Reconnect erneut publiziert
- [x] Boot-Zeit bis "voll betriebsbereit" < 30 Sekunden

**Abhängigkeiten:** US-1.4
**Komponenten:** `main.c`, `core/foundation_init.c`, alle Adapter

---

### US-5.2: Geräte-Verfügbarkeit überwachen
> **Als** Marco **möchte ich** benachrichtigt werden, wenn ein Zigbee-Gerät nicht mehr erreichbar ist,
> **damit** ich leere Batterien oder Defekte schnell bemerke.

**Akzeptanzkriterien:**
- [x] Periodische Availability-Checks für batteriebetriebene Geräte (bei Report)
- [x] Periodische Availability-Checks für netzbetriebene Geräte (aktiver Check, z.B. alle 10 Min)
- [x] Status `available: false` nach konfigurierbarem Timeout
- [x] MQTT-Message bei Statusänderung: `zigbee2mqtt/<name>/availability` → `{"state": "offline"}`
- [x] HA-Entity wird als "unavailable" markiert
- [x] Bei erneutem Kontakt: automatisch wieder "online"

**Abhängigkeiten:** US-2.3
**Komponenten:** `zigbee/zb_availability.c`, `core/adapters/mqtt_adapter.c`

---

### US-5.3: Ressourcenmanagement unter Last
> **Als** Marco **möchte ich**, dass das Gateway auch mit 30+ Zigbee- und 50+ BLE-Geräten stabil läuft,
> **damit** mein Smart Home zuverlässig funktioniert.

**Akzeptanzkriterien:**
- [x] Memory-Pressure-Detection: Warnung bei < 50KB free heap
- [x] Graceful Degradation: BLE-Scanner wird bei Memory-Pressure pausiert
- [x] Zigbee-Koordinator hat höchste Priorität (wird nie deaktiviert)
- [x] Buffer-Pools verhindern Heap-Fragmentierung
- [x] Kein Crash bei vollem Device-Registry
- [x] Monitoring-Daten über MQTT abrufbar

**Abhängigkeiten:** US-2.1, US-3.1
**Komponenten:** `core/memory/memory_manager_ng.c`, `core/monitoring/`, Lifecycle Manager

---

### US-5.4: WiFi/Zigbee/BLE-Koexistenz
> **Als** Marco **möchte ich**, dass WiFi, Zigbee und Bluetooth gleichzeitig störungsfrei funktionieren,
> **damit** alle Protokolle zuverlässig arbeiten.

**Akzeptanzkriterien:**
- [x] 5GHz-WiFi bevorzugt (Vermeidung von 2.4GHz-Interferenz mit Zigbee/BLE)
- [x] Hardware-Koexistenz-Modul aktiv (ESP32-C5 HW-Coex)
- [x] Prioritäten: Zigbee TX > BLE TX > WiFi TX (konfigurierbar)
- [x] Kein spürbarer Latenzanstieg bei gleichzeitiger Nutzung
- [x] MQTT bleibt verbunden während Zigbee-Pairing und BLE-Scan
- [x] Fallback auf 2.4GHz WiFi wenn kein 5GHz verfügbar

**Abhängigkeiten:** US-1.2, US-2.1, US-3.1
**Komponenten:** `wifi/`, `sdkconfig.defaults` (ESP-IDF HW-Coex-Settings)

---

## Phase 6: Wartung & Updates

### US-6.1: OTA-Firmware-Update
> **Als** Anna **möchte ich** die Gateway-Firmware über WLAN aktualisieren,
> **damit** ich das Gerät nicht vom Einsatzort entfernen muss.

**Akzeptanzkriterien:**
- [x] OTA via HTTP-URL: Befehl via MQTT `zigbee2mqtt/bridge/request/ota` → `{"url": "http://..."}`
- [x] OTA via ESPHome (HA-Integration): Update-Button in HA
- [x] Fortschrittsanzeige via MQTT (%)
- [x] Validierung der Firmware vor dem Flashen (Signatur, Größe, Ziel-Chip)
- [x] Dual-Partition: Rollback auf vorherige Version bei Boot-Fehler
- [x] Zigbee-Netzwerk und Geräte bleiben nach Update erhalten
- [x] LED zeigt Update-Status an

**Abhängigkeiten:** US-1.3
**Komponenten:** `ota/`, Partitionstabelle (ota_0/ota_1)

---

### US-6.2: Zigbee-Netzwerk-Backup & Restore
> **Als** Marco **möchte ich** mein Zigbee-Netzwerk sichern und auf ein neues Gateway übertragen können,
> **damit** ich bei einem Hardwaredefekt nicht alle Geräte neu pairen muss.

**Akzeptanzkriterien:**
- [x] Backup via MQTT: `zigbee2mqtt/bridge/request/backup` → Antwort mit Backup-Daten (JSON)
- [x] Backup enthält: Network Key, PAN-ID, Kanal, Geräteliste, Bindings, Gruppen
- [x] Restore via MQTT auf neuem Gateway
- [x] Nach Restore: Geräte verbinden sich automatisch wieder
- [x] Backup kann auch lokal auf SPIFFS gespeichert werden

**Abhängigkeiten:** US-2.1
**Komponenten:** `zigbee/zb_backup.c`, `zigbee/zb_coordinator.c`, `zb_storage` Partition, NVS

---

### US-6.3: Konfiguration ändern (Runtime)
> **Als** Marco **möchte ich** Einstellungen zur Laufzeit ändern können,
> **damit** ich das Gateway nicht für jede Änderung neu flashen muss.

**Akzeptanzkriterien:**
- [x] Konfigurierbar via MQTT: `zigbee2mqtt/bridge/request/config` → `{"log_level": "debug", ...}`
- [x] Änderbare Parameter: Log-Level, Permit Join Timeout, Scan-Intervall, Availability-Timeout
- [x] Änderungen werden in NVS persistiert
- [x] Aktuelle Config abrufbar via MQTT
- [x] Einige Parameter erfordern Neustart (z.B. Zigbee-Kanal) - wird klar kommuniziert

**Abhängigkeiten:** US-1.3
**Komponenten:** `core/bridge/bridge_request_handler.c`, NVS, Kconfig

---

### US-6.4: Log-Zugriff und Debugging
> **Als** Marco **möchte ich** auf die Logs des Gateways zugreifen können,
> **damit** ich Probleme diagnostizieren kann.

**Akzeptanzkriterien:**
- [x] Logs via MQTT: `zigbee2mqtt/bridge/logging` mit konfigurierbarem Level
- [x] Logs via serielle Konsole (USB)
- [x] Log-Level zur Laufzeit änderbar (ERROR, WARN, INFO, DEBUG)
- [x] Strukturierte Logs mit Timestamp, Modul, Level
- [x] Crash-Reports werden nach Neustart via MQTT publiziert

**Abhängigkeiten:** US-1.3
**Komponenten:** `core/monitoring/`, MQTT Logger, ESP-IDF Log-System

---

## Phase 7: Erweiterte Szenarien

### US-7.1: Tuya-Gerät einbinden
> **Als** Marco **möchte ich** auch günstige Tuya-Zigbee-Geräte nutzen können,
> **damit** ich nicht auf teure Markengeräte beschränkt bin.

**Akzeptanzkriterien:**
- [x] Tuya-spezifische Cluster werden erkannt und korrekt interpretiert
- [x] Generischer Tuya-Driver für unbekannte Tuya-Geräte
- [x] Tuya Datapoints (DPs) werden auf Standard-Cluster gemappt
- [x] Fingerbot, Thermostate, Steckdosen, Vorhangmotoren unterstützt
- [x] Custom-Converter für unbekannte Tuya-Geräte via Konfiguration

**Abhängigkeiten:** US-2.2
**Komponenten:** `zigbee/tuya/`, `zigbee/converter/tuya_*.c`

---

### US-7.2: Touchlink-Commissioning
> **Als** Marco **möchte ich** Geräte via Touchlink hinzufügen können,
> **damit** ich z.B. Philips Hue Lampen direkt übernehmen kann.

**Akzeptanzkriterien:**
- [x] Touchlink-Scan startet via MQTT-Befehl
- [x] Gefundene Geräte werden aufgelistet
- [x] Factory-Reset von Touchlink-Geräten möglich
- [x] Gerät wird nach Touchlink normal ins Netzwerk integriert

**Abhängigkeiten:** US-2.1
**Komponenten:** `zigbee/zb_touchlink.c`

---

### US-7.3: Eigenen Device-Converter schreiben
> **Als** Lisa **möchte ich** einen eigenen Converter für ein nicht unterstütztes Gerät schreiben,
> **damit** ich auch exotische Geräte einbinden kann.

**Akzeptanzkriterien:**
- [x] Dokumentierte Converter-API mit Beispiel-Converter
- [x] Converter besteht aus: Identifikation (Model/Manufacturer), Cluster-Mapping, State-Conversion
- [x] Hot-Reload ohne Re-Flash (wenn möglich) oder klare Anleitung zum Hinzufügen
- [x] Template-Converter als Ausgangsbasis
- [x] Converter kann Exposes definieren (welche Entities in HA erscheinen)

**Abhängigkeiten:** US-2.3
**Komponenten:** `zigbee/converter/`, API-Dokumentation

---

### US-7.4: Netzwerksicherheit verwalten
> **Als** Marco **möchte ich** die Sicherheit meines Zigbee-Netzwerks verwalten können,
> **damit** keine unautorisierten Geräte beitreten.

**Akzeptanzkriterien:**
- [x] Permit Join ist standardmäßig deaktiviert
- [x] Network Key Rotation via MQTT-Befehl
- [x] Install Codes für sichere Geräte-Aufnahme
- [x] Liste der Netzwerk-Keys abrufbar (nur für Backup)
- [x] Option: Nur bekannte Geräte erlauben (Whitelist-Modus)

**Abhängigkeiten:** US-2.1
**Komponenten:** `zigbee/zb_coordinator.c`, `zigbee/zb_install_codes.c`, `zigbee/zb_network.c`

---

## Phase 8: Fehlerszenarien

### US-8.1: WiFi-Verbindung verloren
> **Als** Anna **möchte ich**, dass das Gateway bei WiFi-Ausfall die Zigbee-Steuerung lokal weiterführt und sich automatisch wieder verbindet,
> **damit** mein Smart Home nicht komplett ausfällt.

**Akzeptanzkriterien:**
- [x] Zigbee-Netzwerk läuft unabhängig von WiFi weiter
- [x] Direkte Zigbee-Bindings (z.B. Schalter→Lampe) funktionieren weiterhin
- [x] WiFi-Reconnect mit exponential Backoff (1s, 2s, 4s, ..., max 60s)
- [x] MQTT-Messages werden nach Reconnect nachgeliefert (QoS 1+)
- [x] LED zeigt "WiFi getrennt" an
- [x] Events werden im RAM gebuffert (soweit Speicher vorhanden)

**Abhängigkeiten:** US-1.2, US-2.1
**Komponenten:** `wifi/wifi_manager.c`, `mqtt/`, Event Buffer

---

### US-8.2: MQTT-Broker nicht erreichbar
> **Als** Anna **möchte ich**, dass das Gateway auch ohne MQTT-Broker weiterarbeitet,
> **damit** ein Broker-Neustart nicht alles lahmlegt.

**Akzeptanzkriterien:**
- [x] Zigbee-Netzwerk bleibt aktiv (lokale Bindings funktionieren weiter)
- [x] BLE-Scanner sammelt weiter Daten und liefert sie via ESPHome an HA
- [x] ESPHome Native API funktioniert **unabhängig** von MQTT (BLE-Geräte + Diagnose bleiben verfügbar)
- [x] MQTT-Reconnect automatisch mit Backoff
- [x] Nach Reconnect: aktueller State aller Zigbee-Geräte wird publiziert
- [x] Retained Messages stellen sicher, dass HA den letzten bekannten Zigbee-State hat

**Abhängigkeiten:** US-1.3
**Komponenten:** `mqtt/gateway_mqtt.c`, `core/bridge/mqtt_bridge.c`

---

### US-8.3: Speicherknappheit
> **Als** Marco **möchte ich**, dass das Gateway bei Speicherknappheit kontrolliert degradiert statt abzustürzen,
> **damit** die Kernfunktionen erhalten bleiben.

**Akzeptanzkriterien:**
- [x] Lifecycle States: RUNNING → LOW_MEMORY → CRITICAL
- [x] LOW_MEMORY: BLE-Scanner wird pausiert, keine neuen GATT-Verbindungen
- [x] LOW_MEMORY: JSON-Buffer-Größe wird reduziert
- [x] CRITICAL: Nur noch Zigbee-Koordinator und MQTT aktiv
- [x] Warnung via MQTT und HA-Notification
- [x] Automatische Recovery wenn Speicher wieder verfügbar
- [x] Kein Crash durch OOM (alle Allocations werden geprüft)

**Abhängigkeiten:** US-5.3
**Komponenten:** `core/memory/memory_manager_ng.c`, Lifecycle Manager, `core/monitoring/`

---

### US-8.4: Gerät reagiert nicht (Zigbee)
> **Als** Anna **möchte ich** eine klare Rückmeldung bekommen, wenn ein Gerät nicht reagiert,
> **damit** ich weiß, ob das Gerät defekt ist oder nur die Batterie leer.

**Akzeptanzkriterien:**
- [x] Timeout bei Befehlen mit Fehlermeldung via MQTT
- [x] Unterscheidung: "Gerät nicht erreichbar" vs. "Befehl nicht unterstützt"
- [x] Batteriestatus wird separat getrackt
- [x] Letzter erfolgreicher Kontakt wird angezeigt
- [x] Option: Gerät nach X Fehlversuchen als offline markieren

**Abhängigkeiten:** US-2.4
**Komponenten:** `zigbee/zb_callbacks.c`, `zigbee/zb_availability.c`

---

## Phase 9: Factory Reset & Neustart

### US-9.1: Gateway komplett zurücksetzen
> **Als** Anna **möchte ich** das Gateway auf Werkseinstellungen zurücksetzen können,
> **damit** ich bei Problemen einen sauberen Neuanfang machen kann.

**Akzeptanzkriterien:**
- [ ] Factory Reset via physischen Button (z.B. Boot-Button 10s halten)
- [x] Factory Reset via MQTT: `zigbee2mqtt/bridge/request/factory_reset`
- [x] Sicherheitsabfrage: Bestätigung erforderlich (Button: LED-Blink-Countdown, MQTT: Confirm-Response)
- [x] Alle Daten werden gelöscht: WiFi-Credentials, MQTT-Config, Zigbee-Netzwerk, BLE-Geräte, NVS
- [ ] Automatisches Backup vor Reset (auf SPIFFS, falls möglich)
- [x] Gateway startet danach im Captive-Portal-Modus (wie Ersteinrichtung)
- [x] LED zeigt "Reset durchgeführt" Muster

**Abhängigkeiten:** US-1.1
**Komponenten:** NVS, `zb_storage` Partition, `wifi/`, Button-Handler, `led/`

---

### US-9.2: Selektiver Reset einzelner Subsysteme
> **Als** Marco **möchte ich** gezielt nur das Zigbee-Netzwerk ODER nur die WiFi-Konfiguration zurücksetzen können,
> **damit** ich nicht alles verliere wenn nur ein Teilbereich Probleme macht.

**Akzeptanzkriterien:**
- [x] Reset-Optionen via MQTT: `{"subsystem": "zigbee"}`, `{"subsystem": "wifi"}`, `{"subsystem": "ble"}`, `{"subsystem": "mqtt"}`
- [x] Zigbee-Reset: Netzwerk wird aufgelöst, alle Geräte entfernt, neues Netzwerk gebildet
- [x] WiFi-Reset: Credentials gelöscht, Gateway wechselt in Captive-Portal-Modus
- [x] BLE-Reset: Alle bekannten BLE-Geräte und Bindkeys gelöscht
- [x] MQTT-Reset: Broker-Konfiguration gelöscht, retained Messages bereinigt
- [x] Nicht betroffene Subsysteme laufen unterbrechungsfrei weiter

**Abhängigkeiten:** US-1.3
**Komponenten:** NVS (Namespaces), `zigbee/zb_coordinator.c`, `wifi/`, `bluetooth/`

---

## Phase 10: Gerätetausch & Migration

### US-10.1: Defektes Zigbee-Gerät durch identisches ersetzen
> **Als** Anna **möchte ich** einen kaputten Sensor durch ein identisches Modell ersetzen,
> **damit** meine HA-Automationen und Dashboards weiter funktionieren ohne etwas umkonfigurieren zu müssen.

**Akzeptanzkriterien:**
- [ ] Befehl via MQTT: `zigbee2mqtt/bridge/request/device/replace` → `{"old": "0xAABB...", "new": "auto"}`
- [ ] Neues Gerät wird gepairt und übernimmt: Friendly Name, Gruppen-Mitgliedschaften, Bindings
- [ ] HA-Entities bleiben identisch (gleiche Entity-IDs)
- [ ] Altes Gerät wird automatisch aus Registry archiviert (nicht gelöscht)
- [ ] Letzter bekannter State des alten Geräts bleibt als Referenz verfügbar
- [ ] Falls neues Gerät ein anderes Modell ist: Warnung mit Converter-Unterschieden

**Abhängigkeiten:** US-2.3, US-2.8
**Komponenten:** `core/device/device_registry.c`, `core/discovery/ha_discovery_ng.c`, `zigbee/zb_binding.c`

---

### US-10.2: Gateway-Hardware migrieren (altes ESP → neues ESP)
> **Als** Marco **möchte ich** mein komplettes Zigbee-Netzwerk auf ein neues ESP32-C5-Board übertragen,
> **damit** ich bei einem Hardwaredefekt nicht 30+ Geräte neu pairen muss.

**Akzeptanzkriterien:**
- [ ] Full Export via MQTT: `zigbee2mqtt/bridge/request/export` → JSON mit allen Daten
- [ ] Export enthält: Network Key, PAN-ID, Channel, Extended PAN-ID, alle Geräte + Converter, Gruppen, Bindings, Friendly Names, BLE-Geräte + Bindkeys
- [ ] Import auf neuem Gateway: `zigbee2mqtt/bridge/request/import` → JSON
- [ ] Validierung vor Import: Checksumme, Format-Version, Kompatibilitätscheck
- [ ] Nach Import + Reboot: Geräte verbinden sich automatisch wieder (gleicher Network Key)
- [ ] Trockenlauf-Option: `{"dry_run": true}` zeigt was importiert würde ohne zu committen

**Abhängigkeiten:** US-6.2
**Komponenten:** `core/device/device_persistence.c`, `zigbee/zb_coordinator.c`, NVS Export/Import

---

### US-10.3: Gerät archivieren statt löschen
> **Als** Marco **möchte ich** offline-Geräte archivieren können statt sie zu löschen,
> **damit** ich sie reaktivieren kann falls sie zurückkommen (z.B. nach Batteriewechsel).

**Akzeptanzkriterien:**
- [ ] Archiv-Befehl via MQTT: `zigbee2mqtt/bridge/request/device/archive` → `{"id": "0xAABB..."}`
- [ ] Archivierte Geräte belegen keinen aktiven Registry-Speicher
- [ ] Archivierte Geräte sind abrufbar: `zigbee2mqtt/bridge/request/devices/archived`
- [ ] Reaktivierung wenn Gerät erneut joint: automatische Zuordnung anhand IEEE-Adresse
- [ ] Bei Reaktivierung: Friendly Name, Gruppen, Bindings werden wiederhergestellt
- [ ] Auto-Archiv nach konfigurierbarem Timeout (z.B. 90 Tage offline)

**Abhängigkeiten:** US-2.8
**Komponenten:** `core/device/device_registry.c`, `core/device/device_persistence.c`, NVS

---

## Phase 11: Netzwerk-Gesundheit & Optimierung

### US-11.1: Zigbee-Netzwerk heilen
> **Als** Marco **möchte ich** nach einem Stromausfall oder Gerätewechsel das Mesh-Netzwerk reparieren,
> **damit** alle Geräte optimale Routen finden.

**Akzeptanzkriterien:**
- [x] Network Heal via MQTT: `zigbee2mqtt/bridge/request/network_heal`
- [x] Alle Router senden Route Discovery Broadcast
- [x] End Devices suchen nächsten Router neu
- [x] Fortschrittsbericht via MQTT Events (X von Y Geräte aktualisiert)
- [x] Dauer-Schätzung vorab (abhängig von Geräteanzahl)
- [x] Heal kann im Hintergrund laufen ohne normalen Betrieb zu stören

**Abhängigkeiten:** US-2.1, US-2.6
**Komponenten:** `zigbee/zb_coordinator.c`, `zigbee/zb_network.c`

---

### US-11.2: Netzwerk-Topologie anzeigen
> **Als** Marco **möchte ich** die Mesh-Topologie meines Zigbee-Netzwerks sehen,
> **damit** ich schwache Verbindungen und fehlende Router identifizieren kann.

**Akzeptanzkriterien:**
- [x] Topologie abrufbar via MQTT: `zigbee2mqtt/bridge/request/networkmap` → `{"type": "raw"}`
- [x] Daten pro Verbindung: Source, Target, LQI (Link Quality), Relationship (Parent/Child/Sibling)
- [x] Daten pro Gerät: Typ (Coordinator/Router/EndDevice), Hop-Count zum Koordinator
- [x] Format kompatibel mit Zigbee2MQTT-Networkmap (für bestehende HA-Frontends)
- [x] Regelmäßige automatische Aktualisierung (z.B. alle 4 Stunden)
- [x] Warnung bei Geräten mit LQI < 50 oder Hop-Count > 3

**Abhängigkeiten:** US-2.6
**Komponenten:** `zigbee/zb_topology.c`, Neighbor Table, Routing Table

---

### US-11.3: Zigbee-Kanal wechseln
> **Als** Marco **möchte ich** den Zigbee-Kanal wechseln können wenn WiFi-Interferenzen auftreten,
> **damit** die Kommunikation wieder zuverlässig funktioniert.

**Akzeptanzkriterien:**
- [x] Kanal-Scan via MQTT: `zigbee2mqtt/bridge/request/channel_scan` → Bericht über Belegung aller 16 Kanäle
- [x] Empfehlung des besten Kanals basierend auf WiFi-Interferenz und Geräteanzahl
- [x] Kanalwechsel via MQTT: `zigbee2mqtt/bridge/request/channel_change` → `{"channel": 20}`
- [x] Gateway sendet Zigbee Channel Change Command an alle Geräte
- [x] Fortschrittsbericht: welche Geräte erfolgreich gewechselt haben
- [x] Rollback-Option falls zu viele Geräte den Wechsel nicht schaffen
- [x] Warnung: "X Geräte könnten verloren gehen" vor dem Wechsel

**Abhängigkeiten:** US-2.1
**Komponenten:** `zigbee/zb_coordinator.c`, Energy Scan, Channel Manager

---

### US-11.4: Tote Geräte automatisch bereinigen
> **Als** Marco **möchte ich**, dass lang nicht erreichbare Geräte automatisch archiviert werden,
> **damit** die Registry sauber bleibt und Ressourcen frei werden.

**Akzeptanzkriterien:**
- [x] Konfigurierbarer Timeout: nach X Tagen offline → Auto-Archiv (Default: 90 Tage)
- [x] Vor Auto-Archiv: Warnung via MQTT und HA-Notification
- [x] Täglicher Cleanup-Lauf (konfigurierbare Uhrzeit)
- [x] Whitelist: bestimmte Geräte vom Auto-Archiv ausschließen (z.B. saisonale Sensoren)
- [x] Bericht: "X Geräte seit >Y Tagen offline" abrufbar via MQTT

**Abhängigkeiten:** US-5.2, US-10.3
**Komponenten:** `zigbee/zb_availability.c`, `core/device/device_registry.c`

---

## Phase 12: Direkte Geräte-Bindung (ohne Gateway)

### US-12.1: Zigbee Direct Binding erstellen
> **Als** Marco **möchte ich** meinen IKEA-Schalter direkt an eine Philips-Lampe binden,
> **damit** die Lampe auch ohne laufendes Gateway sofort reagiert.

**Akzeptanzkriterien:**
- [x] Binding via MQTT: `zigbee2mqtt/bridge/request/device/bind` → `{"from": "Schalter", "to": "Lampe", "clusters": ["genOnOff", "genLevelCtrl"]}`
- [x] Binding an einzelnes Gerät (1:1) oder Gruppe (1:n)
- [x] Kompatibilitätscheck vor dem Binding (haben beide Geräte die nötigen Cluster?)
- [x] Bestätigung via MQTT Response mit Details
- [x] Bound-Gerät reagiert in < 100ms (direkte Zigbee-Kommunikation)
- [x] Gateway empfängt trotzdem State-Reports (bleibt synchron)

**Abhängigkeiten:** US-2.3
**Komponenten:** `zigbee/zb_binding.c`, ZCL Bind/Unbind Commands

---

### US-12.2: Bindings auflisten und entfernen
> **Als** Marco **möchte ich** alle aktiven Bindings sehen und bei Bedarf entfernen können,
> **damit** ich den Überblick über direkte Verbindungen behalte.

**Akzeptanzkriterien:**
- [x] Liste aller Bindings via MQTT: `zigbee2mqtt/bridge/request/device/bindings` → `{"id": "Schalter"}`
- [x] Pro Binding: Source, Target, Cluster, Typ (Device/Group)
- [x] Unbind via MQTT: `zigbee2mqtt/bridge/request/device/unbind` → `{"from": "Schalter", "to": "Lampe"}`
- [x] Bestätigung dass Unbind auf dem Gerät angekommen ist
- [x] Bulk-Unbind: alle Bindings eines Geräts entfernen (für Gerätetausch)

**Abhängigkeiten:** US-12.1
**Komponenten:** `zigbee/zb_binding.c`, Binding Table Management

---

## Phase 13: Szenen-Management

### US-13.1: Zigbee-Szene erstellen und abrufen
> **Als** Anna **möchte ich** eine "Filmabend"-Szene speichern (Wohnzimmer-Licht gedimmt, Stehlampe aus, LED-Strip blau),
> **damit** ich mit einem Klick die perfekte Stimmung habe.

**Akzeptanzkriterien:**
- [x] Szene erstellen via MQTT: `zigbee2mqtt/bridge/request/scene/add` → `{"name": "Filmabend", "devices": {...}}`
- [x] Szene aus aktuellem Zustand erstellen: `{"name": "Filmabend", "capture": true}` (speichert aktuellen State aller Geräte)
- [x] Szene abrufen via MQTT: `zigbee2mqtt/bridge/request/scene/recall` → `{"name": "Filmabend"}`
- [x] Übergangszeit konfigurierbar: `{"transition": 3}` (3 Sekunden Fade)
- [x] Szene verwendet Zigbee Scene Cluster (on-device gespeichert → schnell, auch ohne Gateway)
- [x] Szene erscheint als Button-Entity in Home Assistant

**Abhängigkeiten:** US-2.4, US-2.9
**Komponenten:** `zigbee/zb_scenes.c`, ZCL Scenes Cluster, `core/discovery/ha_discovery_ng.c`

---

### US-13.2: Szenen verwalten
> **Als** Marco **möchte ich** Szenen auflisten, bearbeiten und löschen können,
> **damit** ich meine Lichtsstimmungen pflegen kann.

**Akzeptanzkriterien:**
- [x] Szenenliste abrufbar via MQTT: `zigbee2mqtt/bridge/request/scenes`
- [x] Szene aktualisieren: einzelne Geräte hinzufügen/entfernen/ändern
- [x] Szene löschen via MQTT: `zigbee2mqtt/bridge/request/scene/remove` → `{"name": "Filmabend"}`
- [x] Szenen werden persistent gespeichert (überleben Gateway-Neustart)
- [x] Szenen können Geräte-Subsets betreffen (nur bestimmte Räume)

**Abhängigkeiten:** US-13.1
**Komponenten:** `zigbee/zb_scenes.c`, NVS/SPIFFS Persistenz

---

## Phase 14: Energie-Monitoring

### US-14.1: Stromverbrauch von Zigbee-Steckdosen tracken
> **Als** Marco **möchte ich** den Stromverbrauch meiner Zigbee-Steckdosen (Sonoff, Tuya) sehen,
> **damit** ich weiß, welche Geräte wie viel Strom fressen.

**Akzeptanzkriterien:**
- [x] Electrical Measurement Cluster und Metering Cluster werden unterstützt
- [x] Werte via MQTT: `{"power": 45.2, "voltage": 230.1, "current": 0.196, "energy": 12.34}`
- [x] Einheiten korrekt: Watt, Volt, Ampere, kWh
- [x] HA-Entities mit `state_class: total_increasing` für Energy Dashboard
- [x] Historische Werte werden von HA gespeichert (nicht vom Gateway)
- [x] Reporting-Intervall konfigurierbar pro Gerät

**Abhängigkeiten:** US-2.5
**Komponenten:** `zigbee/zb_cluster_electrical.c`, `zigbee/cluster_state_ng.c`, `core/discovery/ha_discovery_ng.c`

---

### US-14.2: Batterie-Übersicht aller Geräte
> **Als** Anna **möchte ich** auf einen Blick sehen welche Geräte bald neue Batterien brauchen,
> **damit** ich rechtzeitig Ersatz besorgen kann.

**Akzeptanzkriterien:**
- [ ] Batterie-Übersicht via MQTT: `zigbee2mqtt/bridge/request/battery_report`
- [ ] Sortierung nach Batterie-Level (niedrigste zuerst)
- [ ] Warnschwelle konfigurierbar (Default: 20%)
- [ ] HA-Notification bei kritischem Batteriestand (< 10%)
- [ ] Batterietyp pro Gerät anzeigen (falls vom Gerät gemeldet)
- [ ] Trend-Anzeige: geschätzte Restlaufzeit basierend auf bisherigem Verbrauch

**Abhängigkeiten:** US-2.5
**Komponenten:** `zigbee/zb_reporting.c`, Power Configuration Cluster, `core/bridge/bridge_request_handler.c`

---

## Phase 15: Zeitsynchronisation

### US-15.1: Gateway-Zeit per NTP synchronisieren
> **Als** Marco **möchte ich**, dass das Gateway seine Uhrzeit automatisch per NTP synchronisiert,
> **damit** Timestamps in Logs und Sensordaten korrekt sind.

**Akzeptanzkriterien:**
- [x] NTP-Client startet automatisch nach WiFi-Verbindung
- [x] Konfigurierbarer NTP-Server (Default: `pool.ntp.org`)
- [x] Zeitzone konfigurierbar (Default: UTC, z.B. `Europe/Berlin`)
- [x] Sommerzeit-Umstellung automatisch
- [x] Synchronisations-Status via MQTT: `zigbee2mqtt/bridge/info` enthält `time_synced: true`
- [x] Bei NTP-Ausfall: interne RTC als Fallback, Warnung via MQTT

**Abhängigkeiten:** US-1.2
**Komponenten:** `wifi/`, ESP-IDF SNTP, NVS (Timezone)

---

### US-15.2: Zigbee-Geräte mit korrekter Zeit versorgen
> **Als** Marco **möchte ich**, dass meine Zigbee-Thermostate und Timer die korrekte Uhrzeit haben,
> **damit** ihre Zeitpläne korrekt ausgeführt werden.

**Akzeptanzkriterien:**
- [x] Gateway beantwortet Zigbee Time Cluster Read-Requests
- [x] Periodische Time-Broadcasts an alle Geräte die den Time Cluster unterstützen
- [x] Zeitzone und DST-Offset werden mitgesendet
- [x] Nach NTP-Sync: sofortiger Update an alle Time-Cluster-Geräte
- [x] Logging welche Geräte erfolgreich synchronisiert wurden

**Abhängigkeiten:** US-15.1, US-2.1
**Komponenten:** `zigbee/zb_time_server.c`, ZCL Time Cluster (0x000A)

---

## Phase 16: Sicherheit & Credential-Management

### US-16.1: MQTT-Zugangsdaten ändern ohne Reflash
> **Als** Marco **möchte ich** die MQTT-Zugangsdaten zur Laufzeit ändern können,
> **damit** ich Passwörter regelmäßig rotieren kann.

**Akzeptanzkriterien:**
- [ ] Neue Credentials via aktueller MQTT-Verbindung setzen: `zigbee2mqtt/bridge/request/config/mqtt`
- [ ] Oder via Captive Portal / Web-UI
- [ ] Gateway trennt MQTT, verbindet mit neuen Credentials, bestätigt Erfolg
- [ ] Bei Fehler: Fallback auf alte Credentials mit Fehlermeldung
- [ ] Alte Credentials werden nach erfolgreicher Verbindung überschrieben

**Abhängigkeiten:** US-1.3
**Komponenten:** `mqtt/mqtt_client.c`, NVS, `core/bridge/bridge_request_handler.c`

---

### US-16.2: Zigbee Network Key rotieren
> **Als** Marco **möchte ich** den Zigbee Network Key regelmäßig wechseln,
> **damit** abgehörte Pakete nicht dauerhaft entschlüsselbar bleiben.

**Akzeptanzkriterien:**
- [ ] Key Rotation via MQTT: `zigbee2mqtt/bridge/request/security/rotate_key`
- [ ] Gateway generiert neuen Key und verteilt ihn an alle Geräte (Zigbee Key Transport)
- [ ] Fortschrittsbericht: welche Geräte den neuen Key akzeptiert haben
- [ ] Geräte die den neuen Key nicht annehmen: werden gelistet (müssen ggf. neu gepairt werden)
- [ ] Backup des alten Keys vor Rotation

**Abhängigkeiten:** US-2.1, US-7.4
**Komponenten:** `zigbee/zb_coordinator.c`, Trust Center, NVS

---

### US-16.3: Install Codes für sicheres Pairing
> **Als** Marco **möchte ich** Geräte mit Install Codes pairen,
> **damit** der Network Key nicht im Klartext übertragen wird.

**Akzeptanzkriterien:**
- [ ] Install Code hinzufügen via MQTT: `zigbee2mqtt/bridge/request/install_code/add` → `{"ieee": "0xAABB...", "code": "..."}`
- [ ] Gerät kann nur mit passendem Install Code joinen
- [ ] Pre-Shared Link Key wird aus Install Code abgeleitet
- [ ] Liste aktiver Install Codes abrufbar
- [ ] Install Code wird nach erfolgreichem Join gelöscht

**Abhängigkeiten:** US-2.2
**Komponenten:** `zigbee/zb_coordinator.c`, Trust Center, Install Code Storage

---

## Phase 17: Erweiterte Geräte-Konfiguration (Z2M-Parität)

### US-17.1: Letzter Kontakt-Zeitstempel pro Gerät
> **Als** Marco **möchte ich** sehen wann jedes Gerät zuletzt kommuniziert hat,
> **damit** ich Probleme früh erkenne bevor ein Gerät als offline gilt.

**Akzeptanzkriterien:**
- [ ] `last_seen` Timestamp in jedem State-Update: `{"temperature": 21.5, "last_seen": "2025-01-15T14:32:00Z"}`
- [ ] `last_seen` Format konfigurierbar: ISO 8601, epoch, oder relativ ("vor 5 Minuten")
- [ ] `last_seen` in HA als Diagnose-Entity verfügbar
- [ ] Sortierbare Geräteliste nach `last_seen` (älteste zuerst)

**Abhängigkeiten:** US-2.5, US-15.1
**Komponenten:** `core/device/device_registry.c`, `core/adapters/mqtt_adapter.c`

---

### US-17.2: Reporting-Intervalle pro Gerät konfigurieren
> **Als** Marco **möchte ich** einstellen wie oft ein Sensor seinen Wert meldet,
> **damit** ich die Balance zwischen Aktualität und Batterielebensdauer steuern kann.

**Akzeptanzkriterien:**
- [x] Konfiguration via MQTT: `zigbee2mqtt/<name>/set` → `{"reporting": {"temperature": {"min_interval": 60, "max_interval": 3600, "change": 0.5}}}`
- [x] Min/Max Reporting Interval und Reportable Change pro Attribut
- [x] Voreinstellungen: "Battery-Saver" (selten), "Balanced" (default), "Realtime" (häufig)
- [x] Konfiguration wird auf dem Gerät gespeichert (Zigbee Reporting Configuration)
- [x] Bestätigung dass das Gerät die Config akzeptiert hat

**Abhängigkeiten:** US-2.5
**Komponenten:** `zigbee/zb_reporting.c`, ZCL Configure Reporting

---

### US-17.3: Geräte-Metadaten pflegen
> **Als** Anna **möchte ich** meinen Geräten Raum-Zuordnungen und Notizen hinzufügen können,
> **damit** ich bei 30+ Geräten den Überblick behalte.

**Akzeptanzkriterien:**
- [ ] Custom-Metadaten via MQTT: `zigbee2mqtt/bridge/request/device/options` → `{"id": "...", "options": {"room": "Wohnzimmer", "notes": "Hinter dem Sofa"}}`
- [ ] Metadaten erscheinen in Geräteliste und HA Device Info
- [ ] Vordefinierte Felder: `room`, `floor`, `zone`, `notes`
- [ ] Freie Key-Value-Paare für benutzerdefinierte Metadaten
- [ ] Metadaten werden persistent gespeichert

**Abhängigkeiten:** US-2.7
**Komponenten:** `core/device/device_registry.c`, `core/device/device_persistence.c`

---

### US-17.4: Converter-Override für falsch erkannte Geräte
> **Als** Marco **möchte ich** den automatisch zugewiesenen Converter eines Geräts überschreiben,
> **damit** falsch erkannte Tuya-Geräte korrekt funktionieren.

**Akzeptanzkriterien:**
- [ ] Override via MQTT: `zigbee2mqtt/bridge/request/device/options` → `{"id": "...", "options": {"converter": "tuya_thermostat_v2"}}`
- [ ] Liste verfügbarer Converter abrufbar: `zigbee2mqtt/bridge/request/converters`
- [ ] Override wird persistent gespeichert
- [ ] Nach Override: HA-Entities werden aktualisiert (alte entfernt, neue erstellt)
- [ ] Reset auf Auto-Detection möglich: `{"converter": "auto"}`

**Abhängigkeiten:** US-2.3, US-7.1
**Komponenten:** `zigbee/converter/`, `core/device/device_registry.c`

---

### US-17.5: Multi-Endpoint-Geräte als separate Entities
> **Als** Marco **möchte ich**, dass mein Zigbee-Doppelschalter als zwei separate Schalter in HA erscheint,
> **damit** ich jeden Kanal einzeln steuern kann.

**Akzeptanzkriterien:**
- [ ] Multi-Endpoint-Geräte werden automatisch erkannt (z.B. Endpoint 1 + Endpoint 2)
- [ ] Jeder Endpoint bekommt eigene HA-Entities (z.B. `Schalter_links`, `Schalter_rechts`)
- [ ] MQTT-Topics: `zigbee2mqtt/Schalter/left/set` und `zigbee2mqtt/Schalter/right/set`
- [ ] Beide Endpoints gehören zum selben HA-Device (gruppiert)
- [ ] Endpoint-Benennung konfigurierbar

**Abhängigkeiten:** US-2.3
**Komponenten:** `zigbee/zb_interview.c`, `zigbee/converter/`, `core/discovery/ha_discovery_ng.c`

---

## Phase 18: Crash Recovery & Watchdog

### US-18.1: Automatische Crash-Erkennung und Neustart
> **Als** Anna **möchte ich**, dass das Gateway nach einem Absturz automatisch neu startet und mir den Grund mitteilt,
> **damit** ich nicht manuell eingreifen muss.

**Akzeptanzkriterien:**
- [ ] Hardware-Watchdog: Neustart nach 30s Hänger (konfigurierbar)
- [ ] Task-Watchdog: Erkennt blockierte FreeRTOS-Tasks
- [ ] Nach Crash-Reboot: Crash-Grund wird in NVS gespeichert
- [ ] Crash-Report via MQTT nach Reconnect: `zigbee2mqtt/bridge/event` → `{"type": "crash_report", "reason": "...", "backtrace": "..."}`
- [ ] HA-Notification bei Crash
- [ ] Priorisierter Neustart: Zigbee-Netzwerk zuerst, dann MQTT, dann BLE

**Abhängigkeiten:** US-5.1
**Komponenten:** ESP-IDF Watchdog, Core Dump, `core/monitoring/`, `main.c`

---

### US-18.2: Crash-Historie und Stabilitäts-Monitoring
> **Als** Marco **möchte ich** sehen wie oft und warum das Gateway abgestürzt ist,
> **damit** ich wiederkehrende Probleme identifizieren kann.

**Akzeptanzkriterien:**
- [ ] Crash-Counter in NVS: Anzahl Crashes seit letztem Factory Reset
- [ ] Letzte 5 Crash-Reasons abrufbar via MQTT: `zigbee2mqtt/bridge/request/crash_history`
- [ ] Uptime-Statistik: aktuelle Uptime, längste Uptime, durchschnittliche Uptime
- [ ] Stability-Score als HA-Sensor (z.B. "99.7% Uptime letzte 7 Tage")
- [ ] Warnung bei häufigen Crashes (> 3 in 24h): "Gateway instabil, prüfe Logs"

**Abhängigkeiten:** US-18.1, US-6.4
**Komponenten:** NVS (Crash-Storage), `core/monitoring/`, `core/discovery/ha_discovery_ng.c`

---

## Phase 19: Decommissioning & End-of-Life

### US-19.1: Gateway geordnet abschalten
> **Als** Marco **möchte ich** das Gateway sauber herunterfahren,
> **damit** alle Geräte informiert werden und kein Datenverlust entsteht.

**Akzeptanzkriterien:**
- [ ] Shutdown via MQTT: `zigbee2mqtt/bridge/request/shutdown`
- [ ] Shutdown-Sequenz: State "offline" publizieren → alle Pending-Writes abschließen → NVS flush → Zigbee-Stack stoppen → WiFi trennen → Deep Sleep oder Halt
- [ ] Alle Geräte-States werden vor Shutdown gespeichert
- [ ] HA erhält `offline` Status
- [ ] LED zeigt "Shutdown" Muster
- [ ] Aufwachen via Reset-Button oder Stromzyklus

**Abhängigkeiten:** US-1.3
**Komponenten:** `main.c`, `core/foundation_init.c`, alle Subsysteme

---

### US-19.2: Alle Geräte aus Netzwerk entlassen
> **Als** Marco **möchte ich** vor dem Entsorgen des Gateways alle Geräte aus dem Netzwerk entlassen,
> **damit** sie sich an ein neues Gateway anmelden können.

**Akzeptanzkriterien:**
- [ ] Bulk-Leave via MQTT: `zigbee2mqtt/bridge/request/network/dissolve`
- [ ] Gateway sendet Leave-Request an jedes Gerät einzeln
- [ ] Fortschrittsbericht: X von Y Geräte erfolgreich entlassen
- [ ] Geräte die nicht erreichbar sind: werden aus Trust Center entfernt (können sich nicht mehr verbinden)
- [ ] Nach Dissolve: Zigbee-Netzwerk existiert nicht mehr
- [ ] Factory-Reset der Geräte als Alternative dokumentiert

**Abhängigkeiten:** US-2.8
**Komponenten:** `zigbee/zb_coordinator.c`, Trust Center

---

### US-19.3: Sensible Daten sicher löschen
> **Als** Marco **möchte ich** vor Weitergabe/Entsorgung sicherstellen, dass alle Passwörter und Keys gelöscht sind,
> **damit** niemand auf mein Netzwerk zugreifen kann.

**Akzeptanzkriterien:**
- [ ] Secure Wipe via MQTT: `zigbee2mqtt/bridge/request/secure_wipe` (erfordert physischen Button-Confirm)
- [ ] Löscht: WiFi-Passwort, MQTT-Credentials, Zigbee Network Key, BLE Bindkeys, Install Codes
- [ ] NVS wird vollständig gelöscht (nicht nur Einträge entfernt)
- [ ] `zb_storage` Partition wird überschrieben
- [ ] Bestätigung dass Wipe durchgeführt wurde (LED-Muster)
- [ ] Gateway startet im Captive-Portal-Modus (wie fabrikneu)

**Abhängigkeiten:** US-9.1
**Komponenten:** NVS, Flash-Partitionen, Button-Handler

---

## Abhängigkeitsdiagramm

```
US-1.1 (Flash)
  └→ US-1.2 (WiFi/Captive Portal)
       ├→ US-15.1 (NTP Zeitsync)
       │    └→ US-15.2 (Zigbee Time Cluster)
       │    └→ US-17.1 (Last Seen Timestamps)
       └→ US-1.3 (MQTT Config)
            ├→ US-1.4 (Status prüfen)
            ├→ US-9.2 (Selektiver Reset)
            ├→ US-16.1 (MQTT Credentials ändern)
            ├→ US-19.1 (Shutdown)
            │
            ├→ US-2.1 (Zigbee Netzwerk) ──────────────────────────────────┐
            │    ├→ US-2.2 (Permit Join)                                  │
            │    │    ├→ US-16.3 (Install Codes)                          │
            │    │    └→ US-2.3 (Interview/Erkennung)                     │
            │    │         ├→ US-2.4 (Gerät steuern)                      │
            │    │         ├→ US-2.5 (Sensordaten)                        │
            │    │         │    ├→ US-14.1 (Energie-Monitoring)           │
            │    │         │    ├→ US-14.2 (Batterie-Übersicht)           │
            │    │         │    └→ US-17.2 (Reporting-Intervalle)         │
            │    │         ├→ US-2.6 (Geräteliste)                        │
            │    │         ├→ US-2.7 (Umbenennen)                         │
            │    │         │    └→ US-17.3 (Geräte-Metadaten)             │
            │    │         ├→ US-2.8 (Entfernen)                          │
            │    │         │    ├→ US-10.1 (Gerätetausch)                 │
            │    │         │    ├→ US-10.3 (Archivieren)                  │
            │    │         │    │    └→ US-11.4 (Auto-Cleanup)            │
            │    │         │    └→ US-19.2 (Netzwerk auflösen)            │
            │    │         ├→ US-2.9 (Gruppen)                            │
            │    │         │    └→ US-13.1 (Szenen erstellen)             │
            │    │         │         └→ US-13.2 (Szenen verwalten)        │
            │    │         ├→ US-2.10 (Zigbee OTA)                        │
            │    │         ├→ US-12.1 (Direct Binding)                    │
            │    │         │    └→ US-12.2 (Bindings verwalten)           │
            │    │         ├→ US-17.4 (Converter Override)                │
            │    │         └→ US-17.5 (Multi-Endpoint)                    │
            │    ├→ US-6.2 (Netzwerk-Backup)                              │
            │    │    └→ US-10.2 (Hardware-Migration)                     │
            │    ├→ US-7.2 (Touchlink)                                    │
            │    ├→ US-11.1 (Network Heal)                                │
            │    ├→ US-11.2 (Topologie-Map)                               │
            │    ├→ US-11.3 (Kanal wechseln)                              │
            │    └→ US-16.2 (Key Rotation)                                │
            │                                                             │
            ├→ US-4.2 (ESPHome API) ◄── US-1.2 ◄──────────┐               │
            │    ├→ US-3.1 (BLE Scanner) ─── via ESPHome  │               │
            │    │    ├→ US-3.2 (Xiaomi Sensor)           │               │
            │    │    ├→ US-3.3 (Präsenzerkennung)        │               │
            │    │    └→ US-3.4 (BLE Config)              │               │
            │    └→ US-4.3 (Gateway Diagnose)             │               │
            │                                              │               │
            ├→ US-4.1 (HA Discovery/MQTT) ◄───────────────┤───────────────┘
            │                                              │
            ├→ US-5.3 (Ressourcenmanagement) ◄─────────────┘
            ├→ US-5.4 (Koexistenz) ◄───── US-2.1 + US-3.1
            ├→ US-6.1 (OTA Update)
            ├→ US-6.3 (Runtime Config)
            └→ US-6.4 (Logging)

Querschnitt (alle Phasen):
  US-5.1 (Auto-Recovery) → US-18.1 (Crash Recovery) → US-18.2 (Crash Historie)
  US-5.2 (Availability)
  US-8.1 (WiFi-Verlust)
  US-8.2 (MQTT-Ausfall)
  US-8.3 (Speicherknappheit)
  US-8.4 (Gerät reagiert nicht)
  US-9.1 (Factory Reset) → US-19.3 (Secure Wipe)
```

## Prioritäten

| Priorität | User Stories | Begründung |
|-----------|-------------|------------|
| **P0 - Must Have** | US-1.1–1.4, US-2.1–2.5, US-4.1, US-5.1 | Kernfunktionalität, ohne diese ist das Gateway nicht nutzbar |
| **P1 - Should Have** | US-2.6–2.8, US-3.1, US-3.2, US-4.2, US-4.3, US-5.2, US-6.1, US-8.1–8.4, US-9.1, US-12.1, US-17.1, US-17.5, US-18.1 | Wichtig für täglichen Betrieb und Zuverlässigkeit – US-4.2 ist der primäre BLE→HA Integrationsweg |
| **P2 - Nice to Have** | US-2.9, US-2.10, US-3.3, US-3.4, US-5.3, US-5.4, US-6.2–6.4, US-7.1, US-7.4, US-9.2, US-10.1, US-10.3, US-11.1–11.3, US-13.1, US-14.1, US-14.2, US-15.1, US-16.1, US-17.2–17.4 | Erweiterte Funktionen, verbessern das Erlebnis |
| **P3 - Future** | US-7.2, US-7.3, US-10.2, US-11.4, US-12.2, US-13.2, US-15.2, US-16.2, US-16.3, US-18.2, US-19.1–19.3 | Spezialfälle, erfordern zusätzliche Entwicklung |

## Mapping: User Stories → Verifizierter Code-Status

> **Hinweis:** Dieser Status wurde durch Analyse des tatsächlichen Quellcodes verifiziert
> (nicht nur Dateinamen oder Header). Jeder Status basiert auf gelesenen `.c`-Dateien
> mit konkreten Zeilennummern als Beleg.

### Wichtiger Hinweis: BLE ist per Default DEAKTIVIERT

`CONFIG_BT_SCANNER_ENABLED=n` in `sdkconfig.defaults`. Der gesamte BLE-Code (US-3.x)
existiert und ist vollständig, wird aber zu Stubs kompiliert wenn das Flag nicht aktiviert wird.
Aktivierung erfordert `idf.py menuconfig` → Bluetooth aktivieren.

### Phase 1–8: Kernfunktionalität (VERIFIZIERT)

| US | Feature | Status | Hauptdatei |
|----|---------|--------|------------|
| 1.1 | Flash | ✅ | `scripts/flash.sh` |
| 1.2 | WiFi/Captive Portal | ✅ | `wifi/wifi_captive_portal.c` |
| 1.3 | MQTT Config | ✅ | `mqtt/gateway_mqtt.c` |
| 1.4 | LED-Status | ✅ | `led/led_controller.c` |
| 2.1 | Zigbee Netzwerk | ✅ | `zigbee/zb_coordinator.c` |
| 2.2 | Permit Join | ✅ | `zigbee/zb_coordinator.c` |
| 2.3 | Interview | ✅ | `zigbee/zb_interview.c` |
| 2.4 | Steuerung | ✅ | `zigbee/zb_groups.c` |
| 2.5 | Sensordaten | ✅ | `zigbee/zb_reporting.c` |
| 2.6 | Geräteliste | ✅ | `bridge_request_handler.c` |
| 2.7 | Umbenennen | ✅ | `bridge_request_handler.c` |
| 2.8 | Entfernen | ✅ | `bridge_request_handler.c` |
| 2.9 | Gruppen | ✅ | `zigbee/zb_groups.c` |
| 2.10 | Zigbee OTA | ✅ | `zigbee/zb_ota.c` |
| 3.1 | BLE Scanner | ✅⚠️ | `bluetooth/ble_scanner.c` |
| 3.2 | Xiaomi Parser | ✅⚠️ | `bluetooth/devices/ble_xiaomi.c` |
| 3.3 | Präsenz/Beacon | ✅⚠️ | `bluetooth/devices/ble_beacon.c` |
| 3.4 | BLE Config | ⚠️ | `bluetooth/ble_security.c` |
| 4.1 | HA Discovery | ✅ | `core/discovery/ha_discovery.c` |
| 4.2 | ESPHome API | ✅ | `esphome/esphome_api_server.c` |
| 4.3 | Diagnose-Entities | ✅ | `esphome/esphome_api.c` |
| 5.1 | Auto-Recovery | ✅ | `main.c`, `foundation_init.c` |
| 5.2 | Availability | ✅ | `zigbee/zb_availability.c` |
| 5.3 | Memory Pressure | ✅ | `core/memory/memory_pressure.c` |
| 5.4 | Koexistenz | ✅ | `sdkconfig.defaults` |
| 6.1 | OTA Updates | ✅ | `ota/http_ota.c`, `ota/mqtt_ota.c` |
| 6.2 | Backup/Restore | ✅ | `zigbee/zb_backup.c` |
| 6.3 | Runtime Config | ✅ | `bridge_request_handler.c` |
| 6.4 | MQTT Logging | ✅ | `core/mqtt_logger.c` |
| 7.1 | Tuya | ✅ | `zigbee/tuya/tuya_fingerbot.c` |
| 7.2 | Touchlink | ✅ | `zigbee/zb_touchlink.c` |
| 7.3 | Converter API | ⚠️ | `zigbee/converter/zb_converter.h` |
| 7.4 | Netzwerksicherheit | ✅ | `zigbee/zb_install_codes.c` |
| 8.1 | WiFi Reconnect | ✅ | `wifi/wifi_manager.c` |
| 8.2 | MQTT Reconnect | ✅ | `mqtt/gateway_mqtt.c` |
| 8.3 | OOM Handling | ✅ | `core/memory/graceful_degradation.c` |
| 8.4 | Device Timeout | ✅ | `zigbee/zb_availability.c` |

> **✅⚠️** = Code vollständig, aber mit Einschränkung (z.B. BLE per Default deaktiviert, oder fehlender physischer Button-Trigger)
> **⚠️** = Teilweise implementiert (Details siehe unten)

### Phase 9–19: Lifecycle-Erweiterungen (VERIFIZIERT)

| US | Feature | Status | Hauptdatei |
|----|---------|--------|------------|
| 9.1 | Factory Reset | ✅⚠️ | `bridge_request_handler.c` |
| 9.2 | Selektiver Reset | ✅ | `bridge_request_handler.c` |
| 10.1 | Gerätetausch | ❌ | — |
| 10.2 | Hardware-Migration | ⚠️ | `zigbee/zb_backup.c` |
| 10.3 | Archivieren | ❌ | — |
| 11.1 | Network Heal | ⚠️ | `zigbee/zb_router.c` |
| 11.2 | Topologie-Map | ✅ | `zigbee/zb_topology.c` |
| 11.3 | Kanal wechseln | ✅⚠️ | `bridge_request_handler.c` |
| 11.4 | Auto-Cleanup | ❌ | — |
| 12.1 | Direct Binding | ✅ | `zigbee/zb_binding.c` |
| 12.2 | Bindings verwalten | ✅ | `zigbee/zb_binding.c` |
| 13.1 | Szenen erstellen | ✅ | `zigbee/zb_scenes.c` |
| 13.2 | Szenen verwalten | ✅ | `zigbee/zb_scenes.c` |
| 14.1 | Energie-Monitoring | ✅ | `zigbee/zb_cluster_electrical.c` |
| 14.2 | Batterie-Übersicht | ❌ | — |
| 15.1 | NTP Zeitsync | ✅ | `zigbee/zb_time_server.c` |
| 15.2 | Zigbee Time Cluster | ✅ | `zigbee/zb_time_server.c` |
| 16.1 | MQTT Credentials | ⚠️ | `core/config_manager.c` |
| 16.2 | Key Rotation | ✅ | `zigbee/zb_network.c` |
| 16.3 | Install Codes | ✅ | `zigbee/zb_install_codes.c` |
| 17.1 | Last Seen | ✅ | `core/device/device_registry.c` |
| 17.2 | Reporting Config | ✅ | `zigbee/zb_reporting.c` |
| 17.3 | Geräte-Metadaten | ⚠️ | `bridge_request_handler.c` |
| 17.4 | Converter Override | ❌ | — |
| 17.5 | Multi-Endpoint | ✅ | `zigbee/zb_interview.c` |
| 18.1 | Crash Recovery | ✅ | `core/monitoring/crash_reporter.c` |
| 18.2 | Crash Historie | ⚠️ | `core/monitoring/crash_reporter.c` |
| 19.1 | Shutdown | ⚠️ | `zigbee/zb_coordinator.c` |
| 19.2 | Netzwerk auflösen | ⚠️ | `bridge_request_handler.c` |
| 19.3 | Secure Wipe | ❌ | — |

### Gesamtübersicht (VERIFIZIERT)

| Status | Phase 1–8 | Phase 9–19 | Gesamt |
|--------|-----------|-----------|--------|
| ✅ Verifiziert implementiert | 35 | 15 | **50** |
| ⚠️ Teilweise / mit Einschränkung | 2 | 9 | **11** |
| ❌ Nicht implementiert | 0 | 6 | **6** |
| **Gesamt** | **37** | **30** | **67** |

### Die 6 fehlenden Features

| User Story | Was fehlt | Aufwand |
|------------|-----------|--------|
| **US-10.1** Gerätetausch | Device-Replace-API mit Name/Gruppen/Binding-Übernahme | Mittel (2-3 Tage) |
| **US-10.3** Archivieren | Soft-Delete + Reaktivierung bei Re-Join | Mittel (2-3 Tage) |
| **US-11.4** Auto-Cleanup | Automatisches Entfernen lang-offline Geräte mit Warnung | Klein (1-2 Tage) |
| **US-14.2** Batterie-Übersicht | Aggregations-Query über Device-Registry | Klein (1 Tag) |
| **US-17.4** Converter Override | Override-Feld in device_options + Re-Interview-Trigger | Klein (1-2 Tage) |
| **US-19.3** Secure Wipe | Flash-Partition-Overwrite + NVS-Erase + Button-Confirm | Klein (1-2 Tage) |

### Die 11 teilweise implementierten Features

| User Story | Vorhanden | Was fehlt |
|------------|-----------|-----------|
| **US-3.4** BLE Config | BLE-Encryption + Bonding | Xiaomi MiBeacon Bindkey-Support + ESPHome Service-Call API |
| **US-7.3** Converter API | Vollständiges API + 30 Converter | Entwickler-Dokumentation |
| **US-9.1** Factory Reset | MQTT-Handler + NVS-Erase | Physischer Button-Reset (Long-Press) |
| **US-10.2** Hardware-Migration | Backup/Restore (Binary + JSON) | Dry-Run, Validierung, BLE-Device-Export |
| **US-11.1** Network Heal | Route-Error-Monitoring | Aktiver Heal-Trigger via MQTT |
| **US-11.3** Kanal wechseln | Channel-Change via MQTT | Channel-Scan vorher, Rollback bei Fehler |
| **US-16.1** MQTT Credentials | Config-Manager + NVS | Live-Credential-Swap über aktive Verbindung |
| **US-17.3** Geräte-Metadaten | device_options (retain/QoS/debounce) | room/floor/notes Custom-Felder |
| **US-18.2** Crash Historie | Boot-Count + Reset-Reasons in NVS | Uptime-Statistik, Stability-Score |
| **US-19.1/19.2** Decommissioning | Einzel-Leave + Coordinator-Deinit | MQTT-Shutdown-Command, Bulk-Dissolve |
