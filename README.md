# FlipFlat Panel ESP32 – ASCOM ALPACA

Ein WiFi-fähiges DIY Flat-Field Panel für die Astrofotografie mit ESP32, Servo-Klappe, EL-Folie, Heizung und optionalen Hall-Sensor-Endlagen. Steuerbar über ASCOM Alpaca (WiFi) und USB-Serial.

![Status](https://img.shields.io/badge/Status-Development-yellow)
![Firmware](https://img.shields.io/badge/Firmware-v1.0-blue)
![ASCOM](https://img.shields.io/badge/ASCOM-Alpaca-blue)
![ESP32](https://img.shields.io/badge/ESP32-WROOM--32-teal)
![License](https://img.shields.io/badge/License-MIT-green)

## Credits & Ursprung

Die ursprüngliche Idee, der Schaltungsentwurf und das Hardware-Konzept stammen von **Moritz Mayer**. Das Projekt entstand im Umfeld des **[Dark Matters Discord](https://discord.gg/darkmatters)** – einer der aktivsten Astrofotografie-Communities im deutschsprachigen Raum.

Diese ESP32-Implementierung erweitert das Konzept um:

- ASCOM Alpaca REST API (kabellose Steuerung über WiFi)
- BME280 Umgebungssensor (Temperatur, Feuchte, Druck)
- Automatische Heizungsregelung gegen Taubeschlag
- Optionale Hall-Sensor-Endlagenerkennung
- Integrierte Web-Oberfläche zur Konfiguration

## Übersicht

Das FlipFlat Panel kombiniert drei Funktionen in einem Gerät:

1. **Flip** – Servo öffnet und schließt den Deckel automatisch
1. **Flat** – EL-Folie beleuchtet den Tubus gleichmäßig für Flat-Frames
1. **Heater** – Heizfolie verhindert Taubeschlag auf der EL-Folie

Die Steuerung erfolgt kabellos über ASCOM Alpaca – direkt in N.I.N.A. oder jeder anderen Alpaca-kompatiblen Software. Alternativ ist eine USB-Serial-Steuerung möglich.

### Hauptfunktionen

- **ASCOM Alpaca REST API** über WiFi (Port 11111)
- **Alpaca Discovery** (automatische Geräteerkennung im Netzwerk)
- **USB-Serial** als Fallback und für Debugging (57600 Baud)
- **Web-Oberfläche** für Einstellungen (erreichbar im Browser)
- **EL-Folien-Dimmung** (0–255) mit kalibrierbarem Mindest-PWM
- **Heizungsregelung** mit BME280 (Schwellwert + Hysterese)
- **Hall-Sensor-Endlagen** (optional, für sichere Positionserkennung)
- **Einstellbare Servo-Endpunkte** (500–2500 µs)
- **SoftClose** (Cosinus-Easing für sanfte Bewegung)
- **NVS-Speicherung** (alle Einstellungen persistent im Flash)
- **Dual-WiFi** (AP-Modus zur Konfiguration + STA-Modus im Betrieb)

## Hardware

### Stückliste (BOM)

|Bauteil              |Beschreibung                            |Anzahl|Hinweis                 |
|---------------------|----------------------------------------|------|------------------------|
|ESP32-WROOM-32       |DevKit V1 (oder ESP32-S3 für Produktion)|1     |38-Pin Version empfohlen|
|Servo                |SG90 oder MG90S                         |1     |5V, 180°                |
|EL-Folie + Inverter  |Flat-Field-Beleuchtung                  |1     |12V DC Inverter         |
|Heizfolie            |Gegen Taubeschlag                       |1     |12V, 5-10W              |
|IRLZ44N              |Logic-Level N-MOSFET                    |2     |**Nicht** IRFZ44N!      |
|BME280               |Temperatur/Feuchte/Druck                |1     |I2C Breakout (3.3V)     |
|A3144 Hall-Sensor    |Digitaler Hall-Schalter                 |2     |Optional, mit Magneten  |
|AMS1117-3.3V         |Spannungsregler                         |1     |12V → 3.3V für ESP32    |
|10kΩ Widerstand      |Pull-Down MOSFET-Gates                  |2     |                        |
|10kΩ Widerstand      |Pull-Up Hall-Sensoren                   |2     |Optional                |
|Neodym-Magnete       |Für Hall-Sensoren                       |2     |Optional, am Deckel     |
|DC-Buchse 5.5/2.1mm  |12V Eingang                             |1     |                        |
|Kondensator 100µF/25V|Entstörung 12V                          |1     |Elektrolyt              |
|Kondensator 100nF    |Entstörung 3.3V                         |1     |Keramik                 |

### Pin-Belegung ESP32-WROOM-32

|GPIO|Funktion  |Richtung|Beschreibung                                  |
|----|----------|--------|----------------------------------------------|
|18  |Servo     |Output  |PWM via LEDC Channel 0                        |
|19  |EL-Folie  |Output  |PWM via LEDC Channel 1 (IRLZ44N Gate)         |
|23  |Heizung   |Output  |PWM via LEDC Channel 2 (IRLZ44N Gate)         |
|21  |I2C SDA   |I/O     |BME280 Datenleitung                           |
|22  |I2C SCL   |Output  |BME280 Taktleitung                            |
|34  |Hall Open |Input   |Endlage OFFEN (Input-Only, ext. Pull-Up)      |
|35  |Hall Close|Input   |Endlage GESCHLOSSEN (Input-Only, ext. Pull-Up)|


> **Wichtig:** GPIO 34 und 35 sind Input-Only-Pins auf dem ESP32 und haben **keine internen Pull-Ups**. Externe 10kΩ Pull-Up-Widerstände nach 3.3V sind erforderlich, wenn Hall-Sensoren verwendet werden.

### Schaltplan

```
                          12V DC Eingang
                               │
                    ┌──────────┼──────────┐
                    │          │          │
                 ┌──┴──┐    ┌─┴─┐     ┌──┴──┐
                 │100µF│    │AMS│     │     │
                 │ 25V │    │1117│     │     │
                 └──┬──┘    │3.3V│     │     │
                    │       └─┬─┘     │     │
                    │     100nF│       │     │
                   GND    ┌──┴──┐    │     │
                          │3.3V │    │     │
                          └──┬──┘    │     │
                             │       │     │
                         ┌───┴───┐   │     │
                         │ ESP32 │   │     │
                         │WROOM32│   │     │
                         │       │   │     │
              GPIO 19 ───┤       │   │     │
                    │    │       │   │     │
                  ┌─┴─┐  │       │   │     │
                  │10k│  │       │   │     │
                  └─┬─┘  │       │   │     │
                    │    │       │   │     │
                   GND   │       │  12V    12V
                         │       │   │     │
              GPIO 19 ───┼───────┼───┼─ Gate (IRLZ44N #1 - EL)
                         │       │   │     │
                         │       │   │  EL-Inverter(+)
                         │       │   │     │
                         │       │   └─ Drain
                         │       │      │
                         │       │    Source
                         │       │      │
                         │       │     GND
                         │       │
              GPIO 23 ───┼───────┼─── Gate (IRLZ44N #2 - Heizung)
                    │    │       │      │
                  ┌─┴─┐  │       │   Heizfolie(+)──── 12V
                  │10k│  │       │      │
                  └─┬─┘  │       │    Drain
                    │    │       │      │
                   GND   │       │    Source
                         │       │      │
              GPIO 18 ───┤       │     GND
                    │    │       │
                 Servo───┤       │
                         │       │
              GPIO 21 ───┤ SDA   │
              GPIO 22 ───┤ SCL   │
                         │  │    │
                       ┌─┴──┴─┐  │
                       │BME280│  │
                       │ 3.3V │  │
                       └──────┘  │
                                 │
              GPIO 34 ───┬───────┘
                    │    │
                  ┌─┴─┐  │
              3.3V┤10k│  │
                  └─┬─┘  │
                    │    │
              Hall Open  │
              (A3144)    │
                    │    │
                   GND   │
                         │
              GPIO 35 ───┤
                    │    │
                  ┌─┴─┐  │
              3.3V┤10k│  │
                  └───┘  │
                    │    │
              Hall Close │
              (A3144)    │
                    │    │
                   GND   │
                         │
                    └────┘
```

### MOSFET-Beschaltung (Detail)

Jeder der zwei IRLZ44N MOSFETs schaltet die Masse-Seite der Last:

```
ESP32 GPIO ──┬─── Gate (IRLZ44N)
             │
           ┌─┴─┐
           │10k│  Pull-Down (hält MOSFET
           └─┬─┘  beim Boot zuverlässig AUS)
             │
            GND

12V ──── Last (+) ──── Drain (IRLZ44N)
                         │
                       Source
                         │
                        GND
```

> **Warum IRLZ44N statt IRFZ44N?** Der ESP32 hat 3.3V GPIO. Der IRFZ44N braucht mindestens 5V am Gate und schaltet bei 3.3V überhaupt nicht durch. Der IRLZ44N ist ein Logic-Level-MOSFET mit einer Gate-Schwelle von 1-2V und funktioniert zuverlässig mit 3.3V.

### Hall-Sensor-Montage (Optional)

Die Hall-Sensoren A3144 sind digitale Schalter: Wenn ein Magnet in der Nähe ist, gibt der Sensor LOW aus. Zwei kleine Neodym-Magnete werden am Deckel befestigt, so dass sie bei geöffnetem und geschlossenem Zustand jeweils einen Sensor auslösen.

```
Deckel OFFEN:    [Magnet] ←→ [Hall-Sensor OPEN an GPIO 34]
Deckel GESCHLOSSEN: [Magnet] ←→ [Hall-Sensor CLOSE an GPIO 35]
```

**Verdrahtung pro Hall-Sensor:**

```
3.3V ─── 10kΩ ──┬── GPIO 34 oder 35
                │
          A3144 OUT
                │
          A3144 GND ── GND
          A3144 VCC ── 3.3V
```

## Software

### Voraussetzungen

- **Arduino IDE 2.x** mit ESP32 Board Support Package
- **Board:** ESP32 Dev Module (oder ESP32-S3 Dev Module)
- **Libraries** (über Library Manager installieren):
  - `Adafruit BME280 Library` (+ Adafruit Unified Sensor)
  - `ESP32Servo`

### Firmware flashen

1. Arduino IDE öffnen
1. `firmware/FlipFlatPanel_ESP32.ino` öffnen
1. Board: **ESP32 Dev Module**
1. Upload Speed: **921600**
1. Flash Mode: **QIO**
1. Partition Scheme: **Default 4MB with spiffs**
1. COM-Port wählen
1. **Hochladen**

### Erster Start

Nach dem Flashen:

1. Der ESP32 startet ein eigenes WLAN:
- SSID: `FlipFlatPanel`
- Passwort: `flatfield`
1. Verbinde dich mit diesem WLAN
1. Öffne im Browser: `http://192.168.4.1:11111`
1. Konfiguriere dein Heim-WLAN (SSID + Passwort)
1. Nach dem Speichern startet der ESP32 neu und verbindet sich mit deinem WLAN
1. Die IP-Adresse wird im Serial Monitor angezeigt (57600 Baud)

### Web-Oberfläche

Die integrierte Web-Oberfläche zeigt:

- Live-Status (Cover, Helligkeit, Temperatur, Heizung, Hall-Sensoren)
- WiFi-Konfiguration
- Servo-Einstellungen mit Test-Buttons
- Heizungseinstellungen
- Hall-Sensor-Konfiguration

### Verwendung in N.I.N.A. (Alpaca)

1. N.I.N.A. starten
1. **Geräte → Flatpanel**
1. Gerät suchen: **Alpaca** als Quelle wählen
1. Der ESP32 wird automatisch im Netzwerk gefunden (Alpaca Discovery)
1. **FlipFlat Panel** auswählen
1. Verbinden

Alternativ: Manuell die IP-Adresse und Port (11111) eingeben.

## ASCOM Alpaca REST API

### Basis-URL

```
http://<ESP32-IP>:11111/api/v1/covercalibrator/0/
```

### Standard-Endpunkte (ASCOM CoverCalibrator V2)

|Methode|Endpunkt             |Beschreibung                                  |
|-------|---------------------|----------------------------------------------|
|GET    |`/coverstate`        |Cover-Status (0-5)                            |
|GET    |`/covermoving`       |Bewegt sich der Cover?                        |
|PUT    |`/opencover`         |Cover öffnen                                  |
|PUT    |`/closecover`        |Cover schließen                               |
|PUT    |`/haltcover`         |Bewegung stoppen                              |
|GET    |`/calibratorstate`   |Calibrator-Status (0-5)                       |
|GET    |`/calibratorchanging`|Ändert sich der Calibrator?                   |
|GET    |`/brightness`        |Aktuelle Helligkeit                           |
|GET    |`/maxbrightness`     |Maximale Helligkeit (255)                     |
|PUT    |`/calibratoron`      |Calibrator einschalten (Parameter: Brightness)|
|PUT    |`/calibratoroff`     |Calibrator ausschalten                        |

### Erweiterte Endpunkte (FlipFlat-spezifisch)

|Methode|Endpunkt   |Beschreibung                      |
|-------|-----------|----------------------------------|
|GET    |`/status`  |Kompletter Gerätestatus als JSON  |
|GET/PUT|`/settings`|Alle Einstellungen lesen/schreiben|
|GET/PUT|`/wifi`    |WiFi-Konfiguration                |

### Management API

|Methode|Endpunkt                          |Beschreibung              |
|-------|----------------------------------|--------------------------|
|GET    |`/management/apiversions`         |Unterstützte API-Versionen|
|GET    |`/management/v1/description`      |Server-Beschreibung       |
|GET    |`/management/v1/configureddevices`|Konfigurierte Geräte      |

### Beispiel: Helligkeit setzen

```bash
curl -X PUT http://192.168.1.100:11111/api/v1/covercalibrator/0/calibratoron \
  -d "Brightness=128&ClientID=1&ClientTransactionID=1"
```

### Beispiel: Status abfragen

```bash
curl http://192.168.1.100:11111/api/v1/covercalibrator/0/status
```

Antwort:

```json
{
  "coverState": 3,
  "brightness": 128,
  "temperature": 18.5,
  "humidity": 62.3,
  "pressure": 1013.2,
  "heaterActive": false,
  "heaterTarget": 20.0,
  "servoOpen": 2500,
  "servoClose": 540,
  "servoSpeed": 5,
  "softClose": true,
  "minPWM": 128,
  "wifiRSSI": -45,
  "firmware": "1.0"
}
```

## Serielles Protokoll (USB)

Baudrate: **57600**, 8N1, Zeilenende: `\n`

Vollständig kompatibel zum Arduino Nano Firmware v3.0, plus neue Befehle:

### Neue Befehle (ESP32)

|Befehl                      |Antwort                         |Beschreibung                 |
|----------------------------|--------------------------------|-----------------------------|
|`COMMAND:TEMPERATURE`       |`RESULT:TEMPERATURE:18.5`       |Aktuelle Temperatur          |
|`COMMAND:HUMIDITY`          |`RESULT:HUMIDITY:62.3`          |Aktuelle Luftfeuchte         |
|`COMMAND:PRESSURE`          |`RESULT:PRESSURE:1013.2`        |Aktueller Luftdruck          |
|`COMMAND:SETHEATER:1`       |`RESULT:SETHEATER:1`            |Heizung ein/aus              |
|`COMMAND:GETHEATER`         |`RESULT:GETHEATER:1:20.0:18.5:1`|enabled:target:current:active|
|`COMMAND:SETHEATTARGET:22.0`|`RESULT:SETHEATTARGET:22.0`     |Heizungs-Zieltemperatur      |
|`COMMAND:WIFIIP`            |`RESULT:WIFIIP:192.168.1.100`   |Aktuelle WiFi-IP             |
|`COMMAND:SETWIFI:ssid:pass` |`RESULT:SETWIFI:ssid`           |WiFi konfigurieren           |
|`COMMAND:RESTART`           |`RESULT:RESTART:OK`             |ESP32 neu starten            |

## Firmware-Update (OTA)

Nach dem ersten Flashen per USB kann die Firmware kabellos aktualisiert werden – kein USB-Kabel mehr nötig.

### Variante 1: Web-Upload (Browser)

1. Im Browser öffnen: `http://<ESP32-IP>:11111/update`
1. `.bin`-Datei auswählen oder per Drag & Drop hochladen
1. “Firmware hochladen” klicken
1. Der ESP32 startet nach dem Update automatisch neu

Die `.bin`-Datei erzeugt die Arduino IDE unter **Sketch → Kompilierte Binärdatei exportieren**. Sie liegt danach im Projektordner unter `build/`.

### Variante 2: Arduino IDE (WiFi-Upload)

1. Arduino IDE öffnen
1. **Werkzeuge → Port** – dort erscheint `flipflatpanel` als Netzwerk-Port
1. Diesen Port auswählen
1. Ganz normal auf **Hochladen** klicken

Das funktioniert genau wie ein USB-Upload, nur über WiFi. Ideal während der Entwicklung.

> **Sicherheit:** Während eines OTA-Updates werden Servo, EL-Folie und Heizung automatisch abgeschaltet, um Schäden bei einem Fehler zu vermeiden.

## EL-Folien-Kalibrierung

Identisch zum Arduino-Version: Der `SETMINPWM`-Befehl (oder die Web-Oberfläche) kalibriert den Mindest-PWM für den jeweiligen Inverter. Der Wert wird im NVS (Flash) gespeichert.

```
COMMAND:SETMINPWM:150
COMMAND:SETBRIGHTNESS:1    (Test: Leuchtet es stabil?)
COMMAND:SAVE
```

## Heizungsregelung

Die Heizung verwendet eine einfache Schwellwert-Steuerung mit Hysterese:

- **Zieltemperatur** (z.B. 20°C) einstellbar
- **Hysterese** (z.B. 2°C) verhindert schnelles Ein/Aus-Schalten
- Heizung geht **EIN** wenn: Temperatur < (Ziel - Hysterese) = 18°C
- Heizung geht **AUS** wenn: Temperatur ≥ Ziel = 20°C

So bleibt die EL-Folie taufrei, ohne ständig zu heizen.

## Montage-Anleitung

### 1. Elektronik vorbereiten

1. **ESP32 auf Lochraster-Platine** löten (oder Steckbrett für Prototyp)
1. **AMS1117-3.3V Regler** verdrahten:
- Eingang: 12V
- Ausgang: 3.3V → ESP32 3V3-Pin
- Kondensatoren: 100µF am Eingang, 100nF am Ausgang
1. **IRLZ44N #1** (EL-Folie):
- Gate → GPIO 19 + 10kΩ Pull-Down nach GND
- Drain → EL-Inverter Masse
- Source → GND
1. **IRLZ44N #2** (Heizung):
- Gate → GPIO 23 + 10kΩ Pull-Down nach GND
- Drain → Heizfolie Masse
- Source → GND
1. **BME280** I2C anschließen:
- SDA → GPIO 21, SCL → GPIO 22
- VCC → 3.3V, GND → GND
1. **Servo** an GPIO 18 (Signal), 5V (über 12V→5V Regler oder USB), GND

### 2. Hall-Sensoren montieren (optional)

1. **A3144** am Gehäuse befestigen (einer bei Open-Position, einer bei Close-Position)
1. **Neodym-Magnete** am Deckel gegenüber der Sensoren befestigen
1. Verdrahtung: VCC → 3.3V, GND → GND, OUT → GPIO 34/35 mit 10kΩ Pull-Up nach 3.3V
1. **Tipp:** Magnete so positionieren, dass sie bei 2-3mm Abstand zuverlässig auslösen

### 3. Mechanische Montage

1. EL-Folie auf die Innenseite des Deckels kleben
1. Heizfolie auf die Rückseite der EL-Folie kleben
1. BME280 möglichst nahe an der EL-Folie montieren (misst die relevante Temperatur)
1. Kabel mit Kabelbindern oder Spiralschlauch sichern
1. ESP32 + Elektronik in ein wetterfestes Gehäuse (z.B. IP65 Kunststoffgehäuse)

### 4. Inbetriebnahme

1. **12V Netzteil anschließen** (5.5/2.1mm DC-Buchse)
1. **Serial Monitor** öffnen (57600 Baud) – Startmeldungen prüfen
1. **WLAN konfigurieren** (über AP oder Serial)
1. **Servo kalibrieren:**
- Web-Oberfläche öffnen
- Open/Close-Position per Slider anpassen
- “Test Open” / “Test Close” klicken
- Wenn OK → “Speichern”
1. **EL-Folie kalibrieren:**
- MinPWM anpassen bis Helligkeit 1 flackerfrei leuchtet
- Speichern
1. **Heizung konfigurieren:**
- Zieltemperatur einstellen (z.B. 5°C über Umgebung)
- Heizung aktivieren
- Beobachten ob die Regelung funktioniert

## Migration vom Arduino Nano

Wenn du bereits die Arduino-Nano-Version verwendest:

|Arduino Nano            |ESP32                          |
|------------------------|-------------------------------|
|Pin 7 (Servo)           |GPIO 18                        |
|Pin 9 (EL-Folie)        |GPIO 19                        |
|—                       |GPIO 23 (Heizung, neu)         |
|—                       |GPIO 21/22 (BME280, neu)       |
|—                       |GPIO 34/35 (Hall-Sensoren, neu)|
|IRFZ44N / IRLZ44N       |IRLZ44N (zwingend!)            |
|5V Logik                |3.3V Logik                     |
|USB-Serial only         |WiFi + USB-Serial              |
|EEPROM                  |NVS (Flash)                    |
|Serielles Protokoll v3.0|Identisch + neue Befehle       |

Die Controller-App und der ASCOM-Treiber (Serial-Modus) funktionieren auch mit dem ESP32 – das serielle Protokoll ist vollständig kompatibel.

## Fehlerbehebung

|Problem                      |Lösung                                                            |
|-----------------------------|------------------------------------------------------------------|
|ESP32 startet nicht          |12V Versorgung prüfen, AMS1117 korrekt verdrahtet?                |
|WiFi nicht erreichbar        |Serial Monitor: IP-Adresse ablesen. AP-Modus aktiv?               |
|EL-Folie dunkel              |GPIO 19 prüfen, IRLZ44N (nicht IRFZ44N!), SETBRIGHTNESS:255 testen|
|Heizung reagiert nicht       |BME280 vorhanden? GETHEATER prüfen. GPIO 23 + MOSFET prüfen       |
|Hall-Sensor erkennt nicht    |Magnet nah genug? Pull-Up vorhanden? GPIO 34/35 Input-Only!       |
|Alpaca Discovery klappt nicht|Gleich Netzwerk? Port 32227 UDP nicht blockiert?                  |
|Servo zuckt/brummt           |Separate 5V-Versorgung für Servo, GND verbunden?                  |
|BME280 nicht gefunden        |I2C-Adresse: 0x76 oder 0x77? Verkabelung SDA/SCL prüfen           |

## Lizenz

MIT License – siehe <LICENSE> Datei.

## Danksagungen

- **Moritz Mayer** – ursprüngliches Konzept, Schaltung und Hardware-Design
- **[Dark Matters Discord](https://discord.gg/darkmatters)** – Community, Tests und Feedback
- [ASCOM Initiative](https://ascom-standards.org/) für Alpaca und die Platform
- [N.I.N.A.](https://nighttime-imaging.eu/) als beste freie Astrofotografie-Software

-----

**Clear skies! 🔭✨**