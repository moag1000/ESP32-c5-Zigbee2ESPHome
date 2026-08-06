# CLAUDE.md - ESP32-C5 Zigbee HA Native (ESPHome Primary)

> Stand: 2026-07-31. BLE ist projektweit deaktiviert, Converter kommen zur
> Laufzeit aus einer LittleFS-DB. Beides steht unten im Detail -- aeltere
> Dokumente unter `docs/` sind teils noch auf dem Fork-Stand von 2026-02-19.

## Vision
**Hybrid ESPHome Native API + MQTT Gateway** auf ESP32-C5 single-core RISC-V.
ESPHome Native API ist die PRIMARY Home Assistant Integration (Port 6053, Noise encryption).
MQTT ist sekundaer: Bridge-Management, Debug-Logs, Fallback.
Memory-optimiert, saubere Architektur, keine Code-Duplikation.

## BLE: wieder aktiv (2026-08-05)

`CONFIG_BT_ENABLED=y`, NimBLE als Observer/Central, Scanner an.

**Warum die Abschaltung hinfaellig ist.** Hier stand als Begruendung, der C5
halte "unter WiFi+Zigbee-Koexistenzlast keine stabilen GATT-Verbindungen".
Diese Beobachtung entstand, waehrend **WLAN selbst kaputt war** -- weil
`esp_coex_wifi_i154_enable()` erst spaet im Boot lief. Die Diagnose hat also
mutmasslich die Folge statt der Ursache getroffen.

Gemessen, nachdem die Koexistenz vorgezogen wurde:

    WLAN-Assoziation   25,2 s, 0 Fehlversuche   (ohne BLE: 26,5 s)
    Zigbee             online=2, offline=0, ueber den ganzen Lauf
    BLE-Scan           18 Advertisements, 18 Geraete
    interner Heap      65 KB frei (ohne BLE: 122 KB)

Dreifachfunk laeuft also, und BLE kostet WLAN nichts messbares.

**Der Preis ist Speicher, nicht Stabilitaet:** 25 KB statisch, zur Laufzeit
rund 59 KB (NimBLE-Host und Controller-Puffer). Das ist tragbar, seit der
interne Heap von 67 KB auf 122 KB gestiegen ist -- vorher waere es nicht
gegangen.

**Noch offen:** GATT-Verbindungen selbst sind nicht getestet, es stand kein
Geraet dafuer bereit. Was hier belegt ist, ist Scannen unter Dreifachlast.

### Schalten aus Home Assistant

Zwei Schalter, beide `entity_category = config`:

- **BLE Scanner** -- Scannen an/aus (`ble_scanner_start/stop`)
- **BLE Active Scan** -- passiv gegen aktiv (`ble_scanner_set_active_mode`)

Aktives Scannen sendet Scan Requests und konkurriert damit um dasselbe
2,4-GHz-Funkteil wie Zigbee. Deshalb ist es ein Schalter und keine
Compile-Zeit-Entscheidung: der Handel laesst sich so beobachten statt raten.
Passiv ist der Startzustand.

## Drei Module waren nie initialisiert (behoben 2026-08-06)

Home Assistant bekam von gepairten Geraeten keine Messwerte -- Battery, Voltage,
LQI, RSSI standen dauerhaft auf `nan` bzw. 0. Ursache war nicht ein Fehler,
sondern eine Kette:

1. **`zb_reporting_init()` hatte keinen Aufrufer.** Das Modul blieb inert,
   `s_initialized` false. Der einzige Weg zu `zb_reporting_set_defaults()` ging
   ueber den MQTT-Request-Handler und lief damit ins `ESP_ERR_INVALID_STATE`.
   **Kein Geraet hat je Attribut-Reporting konfiguriert bekommen.**
2. **`zb_binding_init()` hatte ebenfalls keinen Aufrufer.** Reports gehen nur an
   Eintraege in der Binding-Tabelle des Geraets -- ohne Binding waere selbst
   konfiguriertes Reporting ins Leere gelaufen.
3. **`zb_topology_init()` lief nur lazy beim ersten Scan.** `zdo_lqi_callback()`
   hat die Nachbarschaftstabelle ausserdem nur geloggt und die LQI-Werte
   verworfen -- `proto.zigbee.lqi/rssi` hatten im ganzen Baum **keinen
   Schreiber**. Deshalb war auch `linkquality` in jeder State-Message 0.

Alle drei werden jetzt in `main.c` vor `zb_coordinator_start()` initialisiert,
damit ihre `EVT_NETWORK_READY`-Abos stehen, bevor der Stack das Event schickt.

**Zwei echte Fehler kamen erst ans Licht, als der Code tatsaechlich lief:**

- `send_configure_reporting()` schrieb den Zahlenwert per `memcpy` in
  `record.reportable_change` -- ein `void *`. Das SDK dereferenziert diesen
  Zeiger; beim Default 0 also eine Null-Dereferenzierung in
  `zb_zcl_put_value_to_packet()`. Betrifft nur analoge Attributtypen, weshalb
  `onOff` (bool, diskret) durchlief und `batteryPercentageRemaining` (U8) das
  Geraet zerlegte.
- `zb_binding.c` nahm **nirgends** den Zigbee-Lock, `zb_topology_request_neighbors()`
  ebenso wenig. Solange nur der Zigbee-Task rief, fiel das nicht auf.

**Der teuerste Fund lag daneben:** 77 Stellen in `main/esphome/esphome_entity_*.c`
pruefen `s_entities->initialized`, **ohne vorher `s_entities` auf NULL zu
pruefen**. Die Entity-Tabelle wird erst bei ~31 s alloziert, Zigbee laeuft ab
~5 s. Jeder Report in diesem Fenster dereferenziert NULL. Der Absturz war
eindeutig: MTVAL 0x0000d874, vier Byte vor dem Ende der 55416 Byte (0xd878)
grossen Struktur -- also das letzte Feld `initialized` bei Basis NULL.

**Clusterliste wird jetzt persistiert.** `persisted_device_t` speicherte
`short_addr`, `endpoint` und `power_info`, aber **nicht die Cluster**. Ein
wiederhergestelltes Geraet wird nie neu interviewt, kam also mit
`cluster_count = 0` zurueck -- und `device_zigbee_has_cluster()` sagte zu allem
nein. Die Felder haengen hinten am Struct, der Ladepfad memsetzt vorher, alte
Records kommen mit `cluster_count = 0` zurueck.

### Gemessen am Geraet

| | vorher | nachher |
|---|---|---|
| Fingerbot Battery | `nan` | **66 %** |
| Fingerbot LQI / RSSI | 0 / 0 | **244 / -34 dBm** |
| Bindings fuer 0x1F0A | 0 | 2 (IAS Zone, genPowerCfg) |
| Absturz bei Report vor Entity-Init | ja | nein |

**Was weiterhin nicht kommt, und warum:** die Konfigurationswerte des Fingerbot
(Sustain Time, Up/Down Movement, Click Count) sind Tuya-Datenpunkte. Laut den
Hardware-Notizen in `zb_tuya.h` antwortet dieses Geraet auf cmd 0x03 (dataQuery)
mit Default Response und **ohne** Datenpunkte; es dumpt nur nach dem Pairing
oder nach einem Moduswechsel. Ein Moduswechsel in HA holt sie also zurueck --
automatisch machen wir das nicht, weil wir den aktuellen Modus nach einem
Neustart nicht kennen und ihn sonst blind ueberschreiben wuerden.

### Der Rest des Sweeps (2026-08-06)

`zb_groups_init()` und `zb_backup_init()` sind jetzt ebenfalls verdrahtet.
Backup war der schaedliche Fall: `bridge_request_handler` erreicht
`zb_backup_process_mqtt_*()` ueber die MQTT-Bridge, und jeder dieser Aufrufe
lief ins `ESP_ERR_INVALID_STATE`. Groups ist die Abhaengigkeit -- das Backup
stellt Gruppenmitgliedschaften wieder her.

Adapter (`mqtt_adapter_init`, `zigbee_adapter_init`, `ble_adapter_init`) sahen im
Sweep tot aus, sind es aber nicht: sie stehen als `.init = ...` in einer
`adapter_ops_t`-Tabelle **in derselben Datei** und werden ueber den Zeiger
gerufen. Ein Sweep, der die eigene Datei ausschliesst, uebersieht das --
derselbe Fehler in beide Richtungen wie beim Map-vs-grep-Vergleich weiter oben.

Echt unbenutzt bleiben (keine Initialisierung *und* kein einziger anderer
Aufruf): `zb_green_power`, `zb_multi_pan`, `zb_hvac_dehumid`, `ble_security`,
`ble_gatt_discovery`, `ble_battery_service`, `ble_esphome_bridge`. Tote Ketten,
keine Reparatur ohne einen Anwendungsfall. `zb_zcl_helpers_init()` loggt nur --
die Funktionen des Moduls werden benutzt und brauchen keine Initialisierung.

### GATT war nie an -- der Lifecycle hat es nur behauptet

`sync_service_states()` markierte `SERVICE_BLE_GATT` als RUNNING, sobald der
**BLE-Manager** initialisiert war. GATT-Client ist aber ein eigenes Modul, und
`ble_gatt_client_init()` ruft niemand. Jeder Phasenwechsel versuchte daher, einen
nie gestarteten Dienst zu stoppen:

    I LIFECYCLE: Stopping ble_gatt (not needed in PAIRING)
    W BLE_GATT: GATT client not initialized
    E LIFECYCLE: Failed to stop ble_gatt: ESP_ERR_INVALID_STATE

Jetzt wird `ble_gatt_client_is_initialized()` gefragt. **Das aendert die offene
GATT-Frage:** hier stand, GATT sei ungetestet, weil kein Geraet bereitstand --
tatsaechlich war der Client nie initialisiert. Ein GATT-Test braucht also zuerst
einen Aufruf von `ble_gatt_client_init()`, nicht nur ein Testgeraet.

### Sockets gehoeren dem Task, der aus ihnen liest

`esphome_api_disconnect_all_clients()` lief aus `handle_device_interviewed()` --
also auf dem Event-Dispatcher -- und rief `close()` auf Deskriptoren, in denen
der jeweilige Client-Task gerade in `recv()` stand. Das zerlegt lwIPs interne
Queues; sichtbar als Load access fault in `xQueueGenericSend()` auf dem
tcpip-Thread.

Der Fehler war alt, aber selten: er braucht ein Interview waehrend eine
HA-Verbindung steht. Die neue Provisionierung loest Re-Interviews absichtlich
aus und hat ihn damit zum Dauerzustand gemacht -- zwei Abstuerze in 75 s.

Jetzt setzt `disconnect_requested` nur ein Flag, und der Client-Task schliesst
seinen eigenen Socket. Das `recv()` hat ohnehin ein Timeout, die Reaktion kommt
also binnen Sekunden. Nachgemessen: 195 s ohne Absturz, danach ein Lauf mit
0 Abstuerzen und **0 Zeilen auf Fehlerstufe**.

## Toter Code: aufgeraeumt (gemessen 2026-08-05)

Es gibt **keinen unbeabsichtigt toten Code mehr**. Uebrig sind 1.868 Zeilen,
die per Konfiguration korrekt nicht im Image landen:

| Modul | LOC | Warum tot |
|-------|-----|-----------|
| `zigbee/zb_router.c` | 1119 | Router-Modus; Geraet laeuft als Coordinator |
| `core/adapters/ble_adapter.c` | 749 | BLE ist projektweit aus |

Vorher waren es rund 10.560 Zeilen in zwoelf Uebersetzungseinheiten. Der Weg
dahin, weil die Einteilung wichtiger ist als die Zahl:

**Geloescht, weil ersetzt statt unfertig** (3.297 Zeilen) -- die Funktion gibt
es woanders und sie laeuft, ein Anschluss haette eine Zweitimplementierung
erzeugt:

- `cluster_state_ng.c/.h` (1000) -- ersetzt durch `device_registry_set_state()`
  / `_merge_state()`. Der Fall, der am meisten Schaden angerichtet hat: diese
  Datei stand hier lange als zentraler State-Mechanismus beschrieben, waehrend
  sie **kein einziger Aufruf** je erreichte.
- `memory_pool.c/.h` (462) -- ersetzt durch `buffer_pool_t` aus
  `memory_manager_ng`. `buffer_pool_t` ist in `memory_manager_ng.h` deklariert,
  nicht in `memory_pool.h` -- letzteren hat nie jemand inkludiert ausser er
  selbst.
- `coex_manager.c/.h` (787) -- ersetzt durch `esp_coex_wifi_i154_enable()`, das
  `main.c` bereits ruft. Der Rest ist BLE-Scan-Duty, und BLE ist aus.

**Verdrahtet, weil vollstaendig und ohne Doppelung** (4.597 Zeilen), je hinter
einem Kconfig-Flag mit Default n, initialisiert aus `zigbee_stack_start()`:

- `CONFIG_ZB_SCENES_ENABLE` -- `zb_scenes.c` (1545)
- `CONFIG_ZB_TOUCHLINK_ENABLE` -- `zb_touchlink.c` (1070)
- `CONFIG_ZB_OTA_ENABLE` -- `zb_ota.c` (1982)

`zb_scenes` brauchte vorher eine Reparatur: `capture_group_device_states()`
speicherte `on=true, level=MAX` als Platzhalter fuer *jedes* Gruppenmitglied.
Eine so gespeicherte Szene enthielt Werte, die niemand hatte, und ihr Recall
haette alle Lampen auf volle Helligkeit gefahren -- schlimmer als ein Fehler,
weil es nach Erfolg aussieht. Jetzt kommt der Zustand aus
`device_registry_state_dup()`; ein Geraet, das noch nie gemeldet hat, faellt
aus der Szene raus statt erfunden zu werden. Farbe nur als `color_temp`, CIE xy
meldet im ganzen Baum niemand.

`zb_ota` hat eine dokumentierte Luecke: der ZCL-OTA-Server ist vollstaendig,
aber der MQTT-Handler wertet das Topic aus und hoert dann auf -- die
IEEE-Aufloesung hinter `/ota/update/` und `/ota/check/` fehlt. Ansteuern also
ueber `zb_ota_notify_device()`, nicht ueber MQTT. Steht auch im Kconfig-Hilfetext.

Vier weitere Module (`event_trace`, `memory_dashboard`, `batch_publisher`,
`adaptive_memory`) wurden schon am 2026-08-02 nach demselben Muster verdrahtet.

### Den Befund reproduzieren

```bash
grep -oE "libmain\.a\([a-z_0-9]+\.c\.obj\)" build/esp32_c5_zigbee2mqtt.map \
  | sed 's/.*(\(.*\))/\1/' | sort -u > /tmp/kept.txt
# gegen die aktuell kompilierten Quellen aus build/compile_commands.json vergleichen
```

Die Map ist die verlaessliche Quelle. Textsuche nach `<modul>_init` taeuscht in
beide Richtungen: Referenzen aus ebenfalls totem Code zaehlen mit, und
Substring-Treffer wie `evt_zb_ota_progress_t` sehen aus wie Aufrufe.

## Device-Registry: haelt nur noch Geraete (entkoppelt 2026-08-04)

Die Registry zaehlt jetzt Geraete:

    Registry = Zigbee-Geraete + BLE-Geraete

ESPHome-Entities liegen in `main/esphome/esphome_entity_mirror.c` mit eigener
Kapazitaet (`CONFIG_ESPHOME_ENTITY_MIRROR_MAX`, Default 128).

**Warum das noetig war.** Vorher legte `esphome_device_registry.c` fuer *jede*
Entity ein virtuelles Geraet an. Live gemessen (2026-08-04), ueber einen
HA-Verbindungsaufbau hinweg:

    t= 3s   registry= 2/64  ( 3%)   esphome_clients=0   zigbee=2
    t=56s   registry=56/64  (87%)   esphome_clients=1   zigbee=2

54 virtuelle Eintraege fuer zwei gepairte Geraete. `CONFIG_MAX_ZIGBEE_DEVICES`
steht auf 50 — mit dieser Auslegung unerreichbar.

**Der Befund, der die Sache entschied:** unter `ESPHOME_PRIMARY_INTEGRATION`
(= Default) hatte der Spiegel *gar keinen Konsumenten*. `handle_device_state_changed()`
im `mqtt_adapter` steigt bei genau diesem Define in Zeile 1 aus, und
`republish_all_device_states()` ueberspringt `DEV_PROTOCOL_VIRTUAL`. Die 54
Slots und 54 Events pro Zustandsaenderung erzeugten also **kein einziges
MQTT-Topic**. Einziger Empfaenger war `esphome_adapter`s eigener Handler, der
virtuelle Geraete nicht filtert — kein Kreislauf (`diagnostics_update()` bricht
bei non-ZIGBEE ab, `update_entity_states()` findet die Keys nicht), aber
vollstaendig verschwendete Arbeit.

Daher: `CONFIG_ESPHOME_ENTITY_MIRROR` ist `default n if ESPHOME_PRIMARY_INTEGRATION`.
Wer ihn dort trotzdem einschaltet, bekommt die Topics jetzt wirklich — vorher
kostete Einschalten Slots und lieferte nichts.

**Neuer Datenweg.** Entities haben kein `device_t`:

    Entity-Zustand -> esphome_entity_mirror_sync_state()
      -> EVT_ESPHOME_ENTITY_STATE (traegt nur den Key)
      -> mqtt_adapter -> esphome_entity_mirror_get() -> zigbee2mqtt/<name>

`_get()` liefert eine unter der Mirror-Sperre gezogene **Kopie**. Den
cJSON-Zeiger ins Event zu legen waere ein Use-after-free, sobald der naechste
`sync_state()` ihn ersetzt — derselbe Fehler, der schon bei `json_state` steckte.

Die Tabelle ist offen adressiert mit linearem Probing und schliesst Luecken per
**Backward-Shift** statt Tombstones. Das ist der eine Teil, der stillschweigend
kaputtgehen kann: ein falsch geschlossener Probe-Lauf macht kollidierte
Eintraege unerreichbar, ohne zu crashen. `test_deletion_keeps_collided_entries_reachable`
in `tests/unit/test_esphome_entity_mirror.c` deckt das ab (Suite: 20 Tests).

**Nachweis, dass die Kopplung weg ist:** kein `device_registry_add()` im Baum
nimmt noch `DEV_PROTOCOL_VIRTUAL`, und im Default-Build wird
`esphome_entity_mirror.c` gar nicht erst kompiliert (0 Treffer in
`build/compile_commands.json`). Der Fuellstand ist damit strukturell begrenzt,
nicht nur beobachtet.

`bridge/state` fuehrt `registry_used` und `registry_max` mit, damit der
Fuellstand ohne Serial-Log sichtbar ist. `device_registry_add()` warnt ab 80 %
statt erst zu scheitern, wenn ein echtes Geraet abgewiesen wird.

Der Zaehler in `bridge/state` und `bridge/info` meldet ausschliesslich
`stats.zigbee_count` — vorher stand dort die Gesamtzahl, weshalb `bridge/state`
56 gepairte Geraete behauptete waehrend `bridge/devices` zwei auflistete.

## Boot-Reihenfolge: Zigbee haengt nicht am Uplink (seit 2026-08-04)

Der Boot lief frueher strikt **WiFi -> MQTT -> Zigbee**. Ohne erreichbaren AP
hiess das: ~37 s Assoziationsversuche, dann `CONFIG_WIFI_CONNECT_GRACE_SEC`
(300 s) Gnadenfrist, dann ein Captive Portal, das
`CONFIG_WIFI_CAPTIVE_PORTAL_TIMEOUT_SEC` (300 s) blockiert und danach
`esp_restart()` rief. Der Koordinator wurde **nie erreicht** — das Geraet war
nicht offline, sondern tot, in einer Neustartschleife alle ~10,6 min.

Jetzt: `zigbee_stack_start()` in `main.c` (PHASE 2b) startet das Radio **vor**
der MQTT-Phase. Verifiziert auf Hardware ohne WLAN:

    I (335166) MAIN: [PHASE 2b] Zigbee Coordinator Initialization
    I (335802) MAIN: [PHASE 3] MQTT Client Initialization
    I (464777) ZB_AVAIL: Status: online=2, offline=0, unknown=0
    W (636141) WIFI: Captive portal timed out — continuing without an uplink
    I (648865) LIFECYCLE: Phase transition: BOOT -> NORMAL

Ein einziger Boot ueber 785 s (vorher Neustart bei ~635 s).

**Bewusst nicht geaendert:** die Assoziation selbst laeuft weiterhin ohne
aktives 802.15.4. `zigbee_stack_start()` steht *nach* dem WiFi-Zeitfenster,
weil das Einschalten der Koexistenz waehrend der Assoziation deren
RF-Bedingungen veraendert — das blind umzustellen waere nicht pruefbar
gewesen.

**Der Portal-Timeout startet nicht mehr neu.** `captive_portal_stop()` setzt
ohnehin STA-Modus und wifi_manager-Auto-Reconnect zurueck; der Neustart hat nur
ein laufendes Zigbee-Netz abgerissen.

**Zwei Folgefehler, die dabei entstanden sind** (beide behoben, aber als Muster
merken): der Auto-`permit_join` fuer persistierte Geraete stand unter dem
Kommentar *"lifecycle is NORMAL by then"* — eine Annahme, die nur galt, solange
Zigbee nach den Netzphasen startete. Er wartet jetzt auf die Phase, mit einem
Budget, das aus `CONFIG_WIFI_CAPTIVE_PORTAL_TIMEOUT_SEC` **abgeleitet** ist; ein
geratener Festwert (3 min) war 118 s zu kurz und hat den Fix wirkungslos
gemacht.

## WiFi braucht aktivierte Koexistenz (geloest 2026-08-05)

**Die Station assoziiert kaum, solange `esp_coex_wifi_i154_enable()` nicht
gelaufen ist.** Das war die Ursache fuer alles, was hier vorher als
"WLAN dauert 6 Minuten" stand.

Gefunden durch Korrelation ueber vier Mitschnitte -- die Verbindung kam jedes
Mal 21-36 s **nach** `zigbee_stack_start()`, und in Laeufen, die vorher endeten,
nie:

    zigbee_stack_start()   Assoziation   Delta
         335.160             370.742     35,6 s
         335.165             370.763     35,6 s
         335.182             356.974     21,8 s
         455.394             484.662     29,3 s
       nie gestartet          keine        --

Wirkung des Vorziehens, Boot bis verbunden:

    vorher:  371 s, 11 Fehlversuche
    nachher:  27 s,  0 Fehlversuche

Bis dahin scannt der Treiber **fehlerfrei** -- er stimmt jeden Kanal ab, passiv
auf den DFS-Kanaelen (52-64, 100-144) und aktiv sonst, exakt regelkonform -- und
hoert **gar nichts**, auch keine Nachbarnetze, waehrend der AP mit -41 dBm
danebensteht.

`main.c` ruft `zigbee_stack_start()` daher **vor** dem WiFi-Verbindungsfenster.
Ein frueherer Kommentar an derselben Stelle behauptete das Gegenteil -- die
Koexistenz waehrend der Assoziation einzuschalten wuerde diese stoeren, das
Funkteil solle der Station gehoeren. Das war geraten und war verkehrt herum.

### Sackgassen, jede einzeln gemessen

Falls das Thema wieder aufkommt -- diese sechs waren es **nicht**:

| Verdacht | Test | Ergebnis |
|----------|------|----------|
| Ergebnisabholung kaputt | `SCAN_DONE`-Ereignis direkt gelesen | Treiber meldet selbst `number=0 status=0` |
| Kanalspringen zu schnell | 1500 ms fest auf einem Kanal | 0 |
| DFS-Kanal falsch behandelt | Kanal->Modus aus dem Treiberlog | lehrbuchgenau richtig |
| Power-Save schlaeft | `PS=NONE` vor dem Scan | 0 |
| HF nicht gesettelt | 5 s Wartezeit nach STA-Start | 0 |
| Passive Verweilzeit zu kurz | `esp_wifi_set_scan_parameters()`, 1000 ms | **schlechter** (51 s statt 27 s) |

Ebenfalls gemessen und verworfen: Kanal-Hinweis in NVS (kein Unterschied, und
`.channel` allein laesst den Treiber weiter aktiv proben), Laenderpolitik
MANUAL (kein Effekt), Reconnect-Backoff von 30 s auf 5 s (23 statt 11 Versuche
bei gleicher Wanduhrzeit).

**Wichtig fuer die Fehlersuche:** `esp_log_level_set("wifi", ESP_LOG_ERROR)` in
`main.c` drosselt den Treiber. Zum Diagnostizieren auf `ESP_LOG_VERBOSE` --
dort steht, welcher Kanal wie gescannt wird und wann der AP erstmals gehoert
wird (`rsn valid ... mac=`, `ap found, mac=`).

## Der Boot-Scan war kaputt (und hat in die Irre gefuehrt)

`log_visible_aps()` uebergab eine `wifi_scan_config_t`, die ausser
`show_hidden` komplett 0 war. Das ist **kein** Vollscan:
`channel_bitmap.ghz_2_channels` bit0 waehlt zwischen *"scan as bitmap"* (0) und
*"bypass this band"* (1), bits 1-14 benennen die Kanaele. Alles 0 heisst also
"scanne per Bitmap, mit leerer Bitmap". Ergebnis:

    W WIFI_MGR: Scan found no access points at all

Das liest sich wie ein Funkproblem und ist keines. ESP-IDFs eigenes
Scan-Beispiel uebergibt `NULL` fuer einen Vollscan und baut eine Bitmap nur fuer
gezielte Kanaele — jetzt genauso.

**Nicht verwechseln:** dass der 2,4-GHz-Pfad funktioniert, steht unabhaengig
fest. 802.15.4 bildet ein Netz auf Kanal 25 und pollt zwei Geraete erfolgreich
(`online=2`). Dafuer braucht es den Scan nicht.

## ESPHome-API: Hello-Laenge war ungeprueft (behoben 2026-08-05)

Remote auslesbarer Pufferueberlauf **vor** jeder Authentifizierung, gefunden
beim Verdrahten der Noise-Tests.

Die Nutzlastlaenge des Hello-Rahmens ist ein 16-Bit-Feld direkt vom Socket,
erreicht also 65535 -- `client->rx_buffer` ist `ESPHOME_RX_BUFFER_SIZE` (1024).
Die Leseschleife wurde von diesem Wert getrieben, ohne Schranke. Drei Bytes
`01 FF FF` plus Daten schrieben damit bis ~64 KB ueber den Puffer hinaus, ueber
`rx_buffer_len` und den **Zeiger** `noise_ctx`, die in `esphome_client_t`
direkt dahinter liegen.

Hello ist die erste Nachricht der Verbindung; der PSK kommt erst beim Handshake
ins Spiel. Es brauchte also keine Zugangsdaten, nur Erreichbarkeit von TCP 6053
-- dem Port, an dem Home Assistant haengt.

Der Handshake-Pfad direkt darunter hatte die entsprechende Pruefung immer. Der
Hello-Pfad nie.

**Wie es gefunden wurde, weil das die uebertragbare Lehre ist:**
`tests/unit/test_esphome_noise.c` lag seit jeher ungebaut im Baum, weil es
gegen Unitys `TEST_CASE`/`TEST_ADD` geschrieben war. Nach der Umstellung fiel
genau ein Test durch -- `hello_invalid_marker` erwartete, dass
`esphome_noise_process_hello()` ein Marker-Byte prueft. Tut es nicht und soll
es nicht: der `0x01`-Frame-Indikator gehoert eine Ebene hoeher. Dieser Frage
nachzugehen fuehrte auf die fehlende Schranke. Ein fehlschlagender Test ist
nicht immer ein Fehler im Pruefling.

Alle uebrigen `recv()`-Aufrufe in `esphome_api_server.c` wurden mitgeprueft und
sind begrenzt.

## Uebertragbar auf andere ESP32-C5-Projekte

Was hier gelernt wurde und anderswo genauso gilt:

1. **Koexistenz zuerst einschalten.** `esp_coex_wifi_i154_enable()` gehoert vor
   den ersten WLAN-Verbindungsversuch, nicht danach. Ohne sie hoert die Station
   praktisch nichts -- siehe oben, 371 s gegen 27 s. Das ist der wichtigste
   Einzelbefund dieses Projekts.

2. **`esp_log_level_set("wifi", ESP_LOG_ERROR)` verbirgt die Diagnose.** Der
   Treiber protokolliert Kanal fuer Kanal, aktiv/passiv, und wann ein AP
   erstmals gehoert wird. Auf `VERBOSE` stellen, sobald WLAN sich seltsam
   verhaelt.

3. **PSRAM liegt meist brach, waehrend interner RAM der Engpass ist.** Hier:
   6 MB PSRAM zu 99,9 % frei gegen 67 KB internen Heap. Ein einziges Objekt
   (die Entity-Tabelle, 54 KB) nach PSRAM zu verschieben hat den freien
   internen Heap auf 121 KB gebracht. Kandidaten findet man so:

   ```bash
   riscv32-esp-elf-nm --print-size --size-sort --radix=d build/<app>.elf \
     | awk '$3=="b" || $3=="B" {print $2, $4}' | sort -rn | head -20
   ```

   Bedingung: kein Zugriff aus einer ISR und keiner mit abgeschaltetem
   Flash-Cache. Task-Kontext mit Mutex ist unproblematisch.

4. **Der Tiefstand liegt woanders als der Dauerwert.**
   `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)` blieb hier bei 30 KB,
   obwohl der eingeschwungene Wert sich verdoppelte -- der engste Moment liegt
   frueh im Boot (Stack-Initialisierung), nicht im Betrieb. Wer Luft schaffen
   will, muss dort messen.

5. **`esp_coex_preference_set()` wird oft vergessen.** Dieses Projekt ruft es
   nicht; bei Dreifachfunk (WiFi + BLE + 802.15.4) legt es fest, wer im
   Zweifel gewinnt.

### BLE: die Abschaltung ist neu zu bewerten

BLE ist hier deaktiviert mit der Begruendung, der C5 halte "unter
WiFi+Zigbee-Koexistenzlast keine stabilen GATT-Verbindungen". Diese Beobachtung
stammt aus einer Zeit, in der **WLAN selbst kaputt war**, weil die Koexistenz
erst spaet im Boot eingeschaltet wurde. Die Diagnose koennte also die Folge
statt der Ursache getroffen haben.

Dafuer spricht ausserdem: der Speicher, der BLE damals zu teuer machte
(~30 KB intern), ist inzwischen da -- 121 KB statt 67 KB frei.

**Nicht nachgewiesen.** Ein sauberer Gegentest waere: BLE einschalten,
Koexistenz weiterhin vor dem Verbindungsfenster, `esp_coex_preference_set()`
bewusst setzen, und GATT ueber Stunden beobachten. Bis dahin bleibt es eine
begruendete Vermutung.

## Woher die Geraetekenntnis kommt

Das Gateway ist nur deshalb mehr als ein Zigbee-Adapter, weil zwei fremde
Projekte die Arbeit gemacht haben, ein Geraet zu *kennen*:

- **zigbee-herdsman-converters** (MIT, Koen Kanters) -- der Grund, warum ein
  gepairtes Geraet als "Fingerbot Plus" mit Mode-Auswahl und Sustain Time
  erscheint statt als Adresse mit ein paar Clustern. Quellen unter
  `tools/zhc/`, daraus erzeugt `tools/z2m_converter_extract.py` die Daten in
  `data/converters/`.
- **zha-device-handlers** (Apache-2.0) -- deckt ab, was z2m nicht hat.
  `tools/zhaquirks/` -> `data/converters_zhaquirks/`.

Beide Lizenzen verlangen erhaltene Hinweise; die Upstream-Lizenzdateien liegen
neben den jeweiligen Quellen und duerfen dort nicht verschwinden. Vollstaendige
Aufstellung in `THIRD_PARTY_NOTICES.md`, eigene Lizenz in `LICENSE`
(Apache-2.0 -- die Dateikoepfe behaupteten das seit jeher, die Datei fehlte).

`index.json` haelt seit 2026-08-05 die tatsaechliche Upstream-Version fest
(`"version": "26.91.0"`) statt `"commit": "unknown"`. Vorher liess sich nicht
sagen, ob ein Geraet fehlt, weil Upstream es nicht kennt oder weil die
Extraktion alt ist.

## Konfigurations-Kette

`CMakeLists.txt` setzt `SDKCONFIG_DEFAULTS` auf **zwei** Dateien:
`sdkconfig.defaults`, danach `sdkconfig.local`. Die lokale Datei ueberschreibt
die Defaults und ist gitignored (enthaelt WLAN-/MQTT-Zugangsdaten im Klartext --
nicht committen).

`sdkconfig.defaults` zu aendern reicht nicht: das erzeugte `sdkconfig` entsteht
nur bei fehlendem `sdkconfig` oder `idf.py set-target` neu. Vorgehen: `sdkconfig`
sichern und loeschen, `build/` weg, neu bauen -- danach immer
`build/config/sdkconfig.h` gegenpruefen. `idf.py reconfigure` regeneriert sie
nicht zuverlaessig.

## Hybrid Architecture

### Integration Modes
| Mode | Config | HA Integration | MQTT |
|------|--------|----------------|------|
| **ESPHome Primary** (default) | `CONFIG_ESPHOME_PRIMARY_INTEGRATION=y` | ESPHome Native API | Bridge status + debug only |
| MQTT Primary (legacy) | `CONFIG_ESPHOME_PRIMARY_INTEGRATION=n` | MQTT Discovery | Full MQTT state/command |

When `CONFIG_ESPHOME_PRIMARY_INTEGRATION=y`:
- MQTT Discovery (`ha_discovery_ng.c`) is **disabled**
- MQTT state publishing (`mqtt_adapter.c`) is **disabled**
- All device entities are exposed via ESPHome Native API
- Zigbee devices appear as **sub-devices** under the gateway (ESPHome 2025.7.0+)

### Data Flow (ESPHome Primary)
```
Command Flow:
  HA --> ESPHome API (port 6053) --> esphome_adapter.c
    --> zb_converter_handle_command() --> ZCL command --> Zigbee device

State Flow:
  Zigbee Report --> zb_callbacks.c --> Event Bus (EVT_DEVICE_STATE_CHANGED)
    --> esphome_adapter.c --> ESPHome entity state update --> HA
```

### ESPHome Sub-Device Support
Each Zigbee device registers as a sub-device under the ESP32-C5 gateway.
The `device_id` field in entity protobuf messages links entities to their parent sub-device.
Sub-device info is provided in `DeviceInfoResponse` via `esphome_api_handlers.c`.

#### Protobuf device_id Field Numbers
| Entity Type | Field # | Entity Type | Field # |
|-------------|---------|-------------|---------|
| Sensor | 14 | Light | 16 |
| BinarySensor | 10 | Cover | 13 |
| Switch | 10 | Fan | 13 |
| TextSensor | 9 | Climate | 26 |
| Number | 14 | Lock | 12 |
| Button | 9 | MediaPlayer | 10 |
| Select | 9 | AlarmPanel | 11 |
| Text | 12 | | |

### ESPHome Entity Mapping (esphome_adapter.c)
| Device Capability | ESPHome Entity | Notes |
|-------------------|----------------|-------|
| `DEV_CAP_TEMPERATURE` | SENSOR | device_class: temperature |
| `DEV_CAP_HUMIDITY` | SENSOR | device_class: humidity |
| `DEV_CAP_PRESSURE` | SENSOR | device_class: pressure |
| `DEV_CAP_BATTERY` | SENSOR | device_class: battery |
| `DEV_CAP_POWER` | SENSOR | device_class: power |
| `DEV_CAP_ENERGY` | SENSOR | device_class: energy |
| `DEV_CAP_VOLTAGE` | SENSOR | device_class: voltage |
| `DEV_CAP_CURRENT` | SENSOR | device_class: current |
| `DEV_CAP_MOTION` | BINARY_SENSOR | device_class: motion |
| `DEV_CAP_CONTACT` | BINARY_SENSOR | device_class: door |
| `DEV_CAP_VIBRATION` | BINARY_SENSOR | device_class: vibration |
| `DEV_CAP_WATER_LEAK` | BINARY_SENSOR | device_class: moisture |
| `DEV_CAP_SMOKE` | BINARY_SENSOR | device_class: smoke |
| `DEV_CAP_BRIGHTNESS` | LIGHT | + color_temp, RGB support |
| `DEV_CAP_ON_OFF` (no brightness) | SWITCH | simple on/off |
| `DEV_CAP_COVER` | COVER | position, tilt |
| `DEV_CAP_FAN` | FAN | speed levels, oscillation |
| `DEV_CAP_CLIMATE` | CLIMATE | mode, target temp |
| `DEV_CAP_LOCK` | LOCK | lock/unlock/open |

## Features
| Feature | Heap | Status |
|---------|------|--------|
| Zigbee Coordinator | ~80KB | done |
| ESPHome Native API (Primary) | ~25KB | done |
| MQTT Bridge (Secondary) | ~15KB | done |
| Captive Portal | ~10KB | done |
| Event Bus | ~2KB | done |
| Device Registry | ~8KB | done |
| Memory Manager NG | ~2KB | done |
| Cluster State NG | -- | **nicht verdrahtet** -- kein Aufrufer, wird wegoptimiert |
| Device Persistence | ~1KB | done |
| OTA Updates (MQTT/HTTP/ESPHome) | ~5KB | done |
| Zigbee-OTA (`zb_ota.c`) | -- | **nicht verdrahtet** -- nie initialisiert |
| Zigbee Groups | ~2KB | done |
| Zigbee Scenes | -- | **nicht verdrahtet** -- keine externen Referenzen |
| Zigbee Direct Binding | ~2KB | done |
| Zigbee Touchlink | -- | **nicht verdrahtet** -- nie initialisiert |
| Network Topology/Heal | ~3KB | done |
| Zigbee Availability Tracker | ~2KB | done |
| WiFi/Zigbee Coexistence | ~1KB | done |
| System Monitor + Crash Reporter | ~2KB | done |
| LED Status Manager | ~1KB | done |
| mmWave Presence Sensor (S3KM1110) | ~1KB | done |
| ESPHome Service Calls | ~1KB | done -- `permit_join`, `remove_device`, `reconfigure_device` |
| BLE Scanner | -- | **deaktiviert** (Code im Baum, nicht kompiliert) |
| ESPHome BLE Proxy | -- | **deaktiviert** |

**Hardware:** 384KB SRAM, 8MB PSRAM.

**Speicherstand, auf Hardware gemessen 2026-08-05** (loest den alten Wert
"~40KB internal free after full init" aus der BLE-Zeit ab):

| | |
|---|---|
| HP SRAM statisch (Linker) | 179 KB von 321 KB = 55,8 % belegt, **142 KB frei** |
| davon `.bss` / `.text` / `.data` | 57,8 KB / 90 KB / 31 KB |
| Interner Heap beim Start | 198 KB gesamt, 147 KB frei |
| **Interner Heap eingeschwungen** | **59-60 KB frei** (Stand 2026-08-06, BLE an) |
| **Tiefststand seit Boot** | **58 KB** |
| davon durch BLE | rund 59 KB (ohne BLE waren es 122 KB) |
| davon durch Groups/Backup/Binding | rund 6 KB (2026-08-06 dazugekommen) |
| PSRAM | 6115 KB gesamt, 6109 KB frei -- praktisch ungenutzt |
| PSRAM `.bss` statisch | 20 KB |
| Flash-Image | 2,1 MB, App-Partition zu 47 % frei |
| CPU eingeschwungen | 30-36 % |

**Zur Zahl 122 KB, die hier lange stand:** sie wurde **ohne BLE** gemessen. Mit
eingeschaltetem BLE waren es schon damals 65 KB (siehe BLE-Abschnitt oben), die
Tabelle nannte aber weiter den BLE-freien Wert -- ein Vergleich, den niemand
gewinnen konnte. Die 59-60 KB von heute sind also keine Verschlechterung um
63 KB, sondern um rund 6 KB gegenueber dem passenden Vergleichswert: Groups,
Backup und Binding sind neu dazugekommen. Ueber 300 s gemessen bleibt der Wert
stabil (kein Leck), CPU 31 %.

**Caveat:** waehrend der Messung war **kein Home-Assistant-Client verbunden**
(`max_clients=2`, 0 aktiv). Der ESPHome-Client mit seiner Entity-Registrierung
ist der groesste Einzelverbraucher; der Wert mit verbundenem HA fehlt noch.

### Wie der Tiefstand von 30 KB auf 121 KB kam

Der eingeschwungene Wert und der Tiefstand hatten **verschiedene Ursachen** --
den Tiefstand zu jagen brauchte einen eigenen Messaufbau: ein Task, der alle
5 ms den internen Heap abtastet und jedes neue Tief mit Zeitstempel meldet.
Ergebnis: der Einbruch lag in **60 Millisekunden** bei 3,2 s, von 190 KB auf
32 KB -- beim Laden des Converter-Index.

Drei Ursachen, alle behoben:

1. `read_file_to_psram()` in `zb_converter_loader.c` allozierte mit
   `MEM_CAP_DEFAULT` -- also intern, entgegen dem eigenen Namen, fuer einen
   Puffer bis 128 KB.
2. cJSON allozierte komplett intern. Es haengt jetzt per `cJSON_InitHooks()`
   an PSRAM (`route_cjson_to_psram()` in `main.c`, vor jedem cJSON-Aufruf).
   Sicher, weil cJSON nie aus einer ISR und nie bei abgeschaltetem
   Flash-Cache benutzt wird.
3. Die Entity-Tabelle (54 KB) liegt in PSRAM statt in `.bss`.

**Dabei zwei Funktionsfehler gefunden**, die nichts mit Speicher zu tun hatten:

- `MAX_INDEX_ENTRIES` stand auf 256, die DB hat **1109 Hersteller**. Der Index
  brach mit "Index full at 256 entries" ab und **853 Hersteller fehlten
  stillschweigend** -- betroffene Geraete fanden nie einen Converter. Jetzt
  2048 Eintraege, Tabelle in PSRAM.
- Danach lief der String-Intern-Pool ueber (512 Strings / 16 KB), Converter
  kamen mit `(null)` als Namen zurueck. Jetzt 4096 / 128 KB, beides in PSRAM.
  Nebeneffekt: der Index laedt in **3,3 s statt 11,4 s**, weil der Pool nicht
  mehr durchprobiert wird.

Vorher/nachher am Geraet des Nutzers: `Bound converter: 0x1F0A -> (null)`
wurde zu `Bound converter: 0x1F0A -> Vibration sensor`.

**Merksatz:** PSRAM lag zu 99,9 % brach, waehrend interner RAM der Engpass war.
Kandidaten findet man mit `riscv32-esp-elf-nm --print-size --size-sort` ueber
die ELF-Datei; Bedingung ist nur, dass nichts aus einer ISR oder bei
abgeschaltetem Cache darauf zugreift.

## ESP-IDF

| Version | Pfad | Beschreibung |
|---------|------|--------------|
| **v6.0.2** | `~/esp/esp-idf-v6` | Tag-Checkout (detached HEAD), Picolibc, MbedTLS v4.x |

### Setup
```bash
source ~/esp/esp-idf-v6/export.sh
```

Der venv liegt unter `~/.espressif/python_env/idf6.0_py3.14_env` und traegt die
**Host-Python-Version im Namen** -- nach einem Python-Upgrade ist der alte venv
unbrauchbar. Symptom: `export.sh` laeuft mit rc=0 durch, danach
`command not found: idf.py`, im Log
`ERROR: ESP-IDF Python virtual environment ... not found`.
Reparatur: `~/esp/esp-idf-v6/install.sh esp32c5` (zieht ~3.4GB).

### Managed Components (Stand 2026-07-31)
`esp-zigbee-lib` 1.6.8, `esp-zboss-lib` 1.6.4, `led_strip` 3.0.3,
`mdns` 1.11.3, `mqtt` 1.1.0, `littlefs` 1.22.3, `cjson` 1.7.19~2.
Aktualisieren mit `idf.py update-dependencies`.

### Merkmale
- **Picolibc** statt Newlib (Newlib-Kompatibilitaetsmodus aktiv)
- **C23 Standard** (gnu23) -- `bool`/`true`/`false` built-in, `static_assert` ohne `<assert.h>`, `nullptr` verfuegbar
- **MbedTLS v4.x** -- PSA Crypto selektiv (Noise, OTA, Install Codes), sonst Legacy mbedTLS
- Warnings: `-Wall` via ESP-IDF Default, **kein** `-Wextra`/`-Werror`
- Bootloader linker: `bootloader.ld` -> `bootloader.ld.in`

## Build
```bash
source ./scripts/setup_env.sh && ./scripts/build.sh && ./scripts/flash.sh
```

## Architecture

### Layer Abstraction
```
+-------------------------------------+
|         Application Layer           |  <- ESPHome Adapter, Commands
+-------------------------------------+
|         Integration Layer           |  <- ESPHome Native API (primary), MQTT (secondary)
+-------------------------------------+
|         Event Bus                   |  <- Event-Driven (State-Propagation, nicht Commands)
+-------------------------------------+
|         Device Abstraction          |  <- device_t, device_registry
+-------------------------------------+
|         Transport Layer             |  <- Zigbee, BLE, WiFi
+-------------------------------------+
```

### Device Model (NG Complete)

NG `device_t` ist Primary. Legacy `zb_device_handler.c` wurde vollstaendig eliminiert (~1.750 LOC geloescht).
Alle Cluster-Handler sind in eigene Dateien migriert:
- `zb_cluster_hvac.c` (Thermostat + Fan Control)
- `zb_cluster_measurement.c` (Illuminance + Pressure + PM2.5)
- `zb_cluster_electrical.c` (Electrical Measurement + Metering)
- `zb_cluster_security.c` (Door Lock + IAS Zone)
- `zb_cluster_closures.c` (Window Covering)
- `zb_cluster_multistate.c` (Multistate Input/Output/Value)

### Converter: Laufzeit-DB statt einkompilierter Definitionen

Die frueheren 27 einkompilierten Converter-Definitionen gibt es nicht mehr. In C
liegen nur noch drei: `conv_generic.c`, `conv_tuya_bridge.c`,
`conv_tuya_fingerbot.c`. Alles andere kommt zur Laufzeit aus einer JSON-DB in
LittleFS.

- Mountpoint `/littlefs`, DB unter `/littlefs/converters`, Einstieg `index.json`
  (Format v2 mit `manufacturers`- und `files`-Objekten)
- Loader: `main/zigbee/converter/zb_converter_loader.c`
- Partition: `spiffs` ab 0x921000, 0x6DF000 gross (siehe `partitions.csv`)
- Quelldaten im Repo unter `data/`: 477 Hersteller-JSONs (z2m 26.91.0),
  `converters_merged/` (133 Dateien, 1360 Hersteller, 6801 Geraete),
  `converters_zhaquirks/` (554)
- **Ausrollen per LittleFS-Abbild statt MQTT.** `CMakeLists.txt` erzeugt mit
  `littlefs_create_partition_image(spiffs build/lfs_root)` ein Partitionsabbild.
  Bewusst **ohne** `FLASH_IN_PROJECT`, damit ein normaler `idf.py flash` nur die
  App schreibt und die DB nie nebenbei ueberbuegelt:

  ```bash
  rm -rf build/lfs_root && mkdir -p build/lfs_root/converters
  cp data/converters_merged/*.json build/lfs_root/converters/
  idf.py build
  python -m esptool --chip esp32c5 -p <port> --flash-size 16MB \
      write-flash 0x921000 build/spiffs.bin
  ```

  **Achtung: das loescht die Geraetezustaende.** `state_persistence.c` legt seine
  Datei in dieselbe LittleFS-Partition. Nach einem DB-Flash meldet der naechste
  Boot `No persisted state file found`, und jedes Geraet steht in Home Assistant
  auf "unbekannt", bis es das naechste Mal von sich aus meldet -- bei einem
  Fingerbot, der nur nach Pairing oder Moduswechsel meldet, also faktisch nie.
  Der Zustand kommt danach von selbst zurueck (verifiziert 2026-08-06:
  `Published cached state for 2 devices`), aber erst nach dem naechsten Report.

  Der MQTT-Weg (`tools/upload_converters.py`) bleibt fuer Teilupdates. Ueber die
  ESPHome-API geht es **nicht** sinnvoll -- sie kennt keinen Dateitransfer, nur
  base64-Stueckelung durch Service-Aufrufe, was bei 4,9 MB unbrauchbar ist.
- Host-Tools in `tools/`: `z2m_converter_extract.py`, `zhaquirks_transpiler.py`,
  `merge_converter_dbs.py`, `validate_converter_db.py`, `upload_converters.py`.
  `tools/zhc/` und `tools/zhaquirks/` sind die eingecheckten Upstream-Quellen.
- DB-Update zur Laufzeit per MQTT:
  `zigbee2mqtt/bridge/request/converter_db/update` ->
  `handle_converter_db_update()` in `bridge_request_handler.c`

**Zwei Fallstricke:**

1. Der Rebind beim Boot passiert in `main.c` (~Zeile 509), nicht in
   `device_persistence.c`. `device_persistence_load_all()` laeuft, bevor die
   Converter registriert sind -- NG-Devices haben danach `converter == NULL`.
   Fallback-Kette: exakter Hersteller+Modell-Treffer ->
   `conv_generic_for_capabilities()` -> gar kein Converter (Entities dann nur
   aus Capabilities).
2. `zb_converter_find()` macht LittleFS-File-I/O und darf in `zb_interview.c`
   **nicht** unter gehaltenem `s_mutex` laufen -- haelt die Sperre zu lange und
   zerlegt die FreeRTOS-Priority-Inheritance auf dem single-core C5 (siehe
   Kommentar bei `zb_interview.c:1746`).

### Event Bus
```c
#include "core/events/event_bus.h"

// Events publizieren
evt_device_state_t evt = { .ieee_addr = addr, .json_state = json };
event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));

// Events abonnieren
event_subscribe(EVT_DEVICE_STATE_CHANGED, on_state_change, NULL);
```

**Event Types:** `EVT_DEVICE_JOINED`, `EVT_DEVICE_LEFT`, `EVT_DEVICE_STATE_CHANGED`,
`EVT_DEVICE_INTERVIEWED`, `EVT_MQTT_CONNECTED`, `EVT_MQTT_DISCONNECTED`,
`EVT_ESPHOME_ENTITY_STATE` (Entities, die kein `device_t` haben)

### Unified Device Model
```c
#include "core/device/unified_device.h"
#include "core/device/device_registry.h"

// Device lookup
device_t *dev = device_registry_get(ieee_addr);
device_t *dev = device_registry_get_by_short_addr(0x1234);

// Device capabilities
if (dev->capabilities & DEV_CAP_TEMPERATURE) { ... }
if (dev->capabilities & DEV_CAP_ON_OFF) { ... }

// Converter access
const void *conv = device_registry_get_converter(dev->id);
```

### Cluster State NG -- NICHT VERDRAHTET

> Stand 2026-08-01: `cluster_state_ng.c` wird von **keiner** Stelle im Projekt
> aufgerufen und komplett wegoptimiert. Die folgende Beschreibung dokumentiert,
> was das Modul kann, nicht was laeuft. Der tatsaechliche State-Weg geht ueber
> `device_registry_set_state()` / `_merge_state()` und den Event-Bus.
```c
#include "zigbee/cluster_state_ng.h"

// State in device_t's cJSON speichern
cluster_state_set_thermostat(dev->id, &thermo_state);
cluster_state_set_electrical(dev->id, &elec_state);

// Generische Updates
cluster_state_update_number(dev->id, "temperature", 21.5);
cluster_state_update_bool(dev->id, "contact", true);

// Event wird automatisch publiziert: EVT_DEVICE_STATE_CHANGED
```

### Device Persistence
```c
#include "core/device/device_persistence.h"

// Laedt Geraete aus NVS, validiert Format
device_persistence_load_all();  // Auto-clear bei ungueltigen Records

// Speichert einzelnes Geraet
device_persistence_save(dev);

// Speichert alle Geraete
device_persistence_save_all();
```

### Memory Manager NG
```c
#include "core/memory/memory_manager_ng.h"

// PSRAM-aware allocation
void *ptr = mem_alloc(size);           // Auto PSRAM for >1KB
void *ptr = mem_ng_calloc(n, size);    // Zero-initialized
mem_ng_free(ptr);

// Buffer pools (in foundation_init.c)
buffer_pool_t *pool = foundation_get_json_pool();  // 4x 2KB
buffer_pool_t *pool = foundation_get_mqtt_pool();  // 8x 512B
```

## Key Files
| Module | Files |
|--------|-------|
| Entry | `main/main.c` |
| Zigbee | `main/zigbee/zb_coordinator.c`, `zb_callbacks.c`, `zb_interview.c`, `zb_command_handler.c` |
| Zigbee Features | `zb_groups.c`, `zb_scenes.c`, `zb_binding.c`, `zb_touchlink.c`, `zb_topology.c` |
| Zigbee Network | `zb_network.c`, `zb_availability.c`, `zb_reporting.c`, `zb_backup.c` |
| Zigbee OTA | `main/zigbee/zb_ota.c` |
| Tuya | `main/zigbee/tuya/zb_tuya.c`, `tuya_fingerbot.c`, `tuya_driver_registry.c` |
| Zigbee Clusters | `zb_cluster_hvac.c`, `zb_cluster_electrical.c`, `zb_cluster_security.c`, `zb_cluster_closures.c`, `zb_cluster_measurement.c`, `zb_cluster_multistate.c` |
| Cluster State | `main/zigbee/cluster_state_ng.c` |
| **ESPHome API** | `main/esphome/esphome_api_server.c`, `esphome_api_handlers.c`, `esphome_api.c` |
| **ESPHome Entities** | `esphome_entity_sensors.c`, `esphome_entity_controls.c`, `esphome_entity_specialized.c`, `esphome_entities_types.h` |
| **ESPHome Adapter** | `main/core/adapters/esphome_adapter.c` -- Bridges event bus + device registry to ESPHome entities |
| ESPHome Services | `main/esphome/esphome_services.c` (Framework), `esphome_gateway_services.c` (die 3 Gateway-Services) |
| ESPHome OTA | `main/esphome/esphome_ota.c` (Port 3232, gestartet aus `esphome_adapter_gateway.c`) |
| ESPHome Entity Mirror | `main/esphome/esphome_entity_mirror.c` (eigene Ablage, **nicht** die Device-Registry) |
| ESPHome BLE | `main/esphome/esphome_ble_proxy.c` (**inaktiv**, BT aus) |
| ESPHome Crypto | `esphome_noise.c`, `esphome_crypto_constants.h` |
| mmWave | `main/mmwave/mmwave_sensor.c` (S3KM1110, UART, 16 Gates) |
| MQTT (Secondary) | `main/mqtt/gateway_mqtt.c`, `main/core/bridge/mqtt_bridge.c` |
| Events | `main/core/events/event_bus.c` |
| Device (NG) | `main/core/device/device_registry.c`, `unified_device.c`, `device_persistence.c` |
| Command Handler | `main/zigbee/zb_command_handler.c` |
| Memory | `main/core/memory/memory_manager_ng.c`, `adaptive_memory.c` |
| Discovery | `main/core/discovery/ha_discovery_ng.c` (disabled when ESPHome primary) |
| Converter (C) | `main/zigbee/converter/converters/` -- nur noch `conv_generic.c`, `conv_tuya_bridge.c`, `conv_tuya_fingerbot.c` |
| Converter (Laufzeit-DB) | `zb_converter_loader.c`, `zb_converter_registry.c`, `zb_converter_fn_registry.c` |
| Quirks | `zb_quirk_engine.c` (Transform-Pipeline + Init-Sequenzen), `zb_custom_quirk.c` |
| Adapters | `main/core/adapters/esphome_adapter.c`, `mqtt_adapter.c`, `zigbee_adapter.c`, `ble_adapter.c` |
| BLE | `main/bluetooth/` (**nicht kompiliert**, `BT_SRCS` haengt an `CONFIG_BT_ENABLED`); aktiv ist nur `ble_stubs.c` |
| OTA | `main/ota/ota_handler.c`, `mqtt_ota.c`, `http_ota.c` |
| WiFi | `main/wifi/wifi_manager.c`, `wifi_captive_portal.c` |
| Monitoring | `main/core/monitoring/system_monitor.c`, `crash_reporter.c`, `perf_metrics.c` |
| Lifecycle | `main/core/lifecycle_manager.c` |
| Module Mgmt | `main/core/memory/module_manager.c` |

## Coding Rules
| Element | Convention |
|---------|------------|
| Functions | `snake_case` |
| Constants | `UPPER_CASE` |
| Static vars | `s_` prefix |
| NG functions | `_ng` suffix |
| Errors | always check `esp_err_t` |

## Critical Patterns
```c
// Zigbee Thread-Safety
esp_zb_lock_acquire(portMAX_DELAY);
esp_err_t ret = esp_zb_zcl_*();
esp_zb_lock_release();

// Resource Cleanup
esp_err_t func(void) {
    resource_t *r1 = NULL, *r2 = NULL;
    r1 = alloc();
    if (!r1) goto cleanup;
    r2 = alloc();
    if (!r2) goto cleanup;
    // ... use resources ...
    ret = ESP_OK;
cleanup:
    free(r2);
    free(r1);
    return ret;
}

// Event Publishing
evt_device_state_t evt = { ... };
event_publish(EVT_DEVICE_STATE_CHANGED, &evt, sizeof(evt));

// Device Access (NG primary)
device_t *dev = device_registry_get_by_short_addr(short_addr);
device_t *dev = device_registry_get(ieee_addr);

// ESPHome Entity Registration (esphome_adapter.c)
// Capabilities -> entity types, device_id links to sub-device
esphome_adapter_register_device(dev);  // auto-maps capabilities to entities
```

## NG Architecture Status

| Komponente | Aufrufe | Status |
|------------|---------|--------|
| Memory Manager NG | 315 | done, 100% |
| Event Bus | 209 (108 publish, 48 subscribe, 53 unsubscribe) | done |
| Device Registry NG | 342 | done, Primary |
| ESPHome Adapter | -- | done, Primary HA integration |
| ESPHome Sub-Devices | -- | done, device_id in all 15 entity types |
| Buffer Pools | 66 | done |
| Direct MQTT in zigbee/ | 0 publish | done, migrated |
| Legacy zb_device_handler.c | ELIMINIERT | done, ~1.750 LOC geloescht |
| Legacy zb_device_get() | 0 | done, vollstaendig migriert |
| Legacy NVS (zb_devices) | auto-erased on boot | done, NG `devices` namespace primary |
| C23 Standard | gnu23 | done, bool/true/false built-in |
| MQTT Discovery | conditional | done, disabled when CONFIG_ESPHOME_PRIMARY_INTEGRATION=y |

## Roadmap

### Core Architecture
- [x] Event Bus System
- [x] Unified Device Model (device_t)
- [x] Device Registry with PSRAM
- [x] Memory Manager NG + Adaptive Memory
- [x] HA Discovery NG (capability-based, conditional on MQTT primary)
- [~] Cluster State NG (cJSON-based) -- fertig, aber nirgends aufgerufen
- [x] Device Persistence NG (auto-clear invalid, power_info)
- [x] Buffer Pool Helpers (pool_json_print/free)
- [x] Zigbee->Event migration (0 direct MQTT in zigbee/)
- [x] MQTT LWT (offline state on unclean disconnect)
- [x] ESP-IDF v6.0 Platform (Picolibc im Compat-Modus, PSA selektiv)
- [x] C23 Standard (gnu23)
- [x] Legacy zb_device_handler.c eliminiert

### ESPHome Native API (Primary Integration)
- [x] ESPHome API Server (port 6053, Noise encryption)
- [x] ESPHome Adapter (event bus -> ESPHome entities)
- [x] Sub-device support (device_id in all 15 entity types)
- [x] Sensor entities (temperature, humidity, pressure, battery, power, energy, voltage, current)
- [x] Binary sensor entities (motion, contact, vibration, water_leak, smoke)
- [x] Light entities (brightness, color_temp, RGB)
- [x] Switch entities (on/off)
- [x] Cover entities (position, tilt)
- [x] Fan entities (speed, oscillation)
- [x] Climate entities (mode, target temp)
- [x] Lock entities (lock/unlock/open)
- [x] CONFIG_ESPHOME_PRIMARY_INTEGRATION Kconfig option
- [x] MQTT discovery/state disabled when ESPHome primary
- [x] ESPHome device info sync (manufacturer, model from interview)
- [ ] ESPHome OTA for Zigbee sub-devices

### Zigbee Features
- [x] Coordinator with auto-network formation
- [x] Device Interview + Converter binding
- [x] Availability Tracker (power-aware timeouts)
- [x] Groups, Direct Binding
- [~] Scenes -- `zb_scenes.c` fertig, aber keine externen Referenzen
- [~] Touchlink (Hue device takeover) -- fertig, aber nie initialisiert
- [x] Network Topology + Heal
- [~] Zigbee OTA -- siehe oben, nie initialisiert
- [x] Tuya driver framework + Fingerbot

### Connectivity
- [x] OTA Updates (MQTT/HTTP/ESPHome)
- [~] Zigbee-OTA -- `zb_ota.c` fertig, aber nie initialisiert
- [x] WiFi Manager (5GHz auto, captive portal)
- [x] WiFi/Zigbee Coexistence
- [~] BLE Scanner (BLE 5.0 extended, Xiaomi, passive) -- Code fertig, aber
      **abgeschaltet**: keine stabilen GATT-Verbindungen auf dem C5
- [~] ESPHome BLE Proxy -- dito, inaktiv

### Converter & Quirks
- [x] Laufzeit-Converter-DB in LittleFS (index.json v2)
- [x] z2m- und zhaquirks-Transpiler (Host-Tools unter `tools/`)
- [x] Quirk-Engine (Transform-Pipeline, Init-Sequenzen)
- [x] Custom-Quirk-Upload + DB-Update per MQTT

### Sensorik
- [x] mmWave-Praesenzsensor S3KM1110 (UART, 16 Gates)

### Monitoring
- [x] System Monitor (heap, CPU, tasks)
- [x] Crash Reporter (boot reason, NVS persistence)
- [x] LED Status Manager (RGB patterns)
- [x] Performance Metrics + Latency Measurement

### Remaining
- [ ] CI/CD Pipeline (Build + Lint + Format)
- [ ] Web Dashboard (HTTP status page)
- [x] ESPHome-Services: `permit_join`, `remove_device`, `reconfigure_device`
- [ ] ESPHome OTA fuer Zigbee-Sub-Devices (`esphome_ota.c` deckt bisher nur das
      Gateway selbst ab, Port 3232)
- [ ] Bootloader-Groesse: 0x5700 von 0x6000 belegt, nur ~2.3KB frei
- [x] Freien internen RAM ohne BLE auf Hardware nachgemessen (67 KB frei, siehe oben)
- [ ] Denselben Wert mit verbundenem Home Assistant messen
- [ ] `docs/` auf den aktuellen Stand ziehen -- grosse Teile sind noch der
      Fork-Stand vom 2026-02-19 und beschreiben BLE als aktiv
