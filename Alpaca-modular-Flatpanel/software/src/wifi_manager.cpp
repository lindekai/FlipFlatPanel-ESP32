#include "wifi_manager.h"

#include "debug_monitor.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "flatpanel_control.h"

static DNSServer dnsServer;
static Preferences preferences;
static const char* AP_SSID = "FlipFlatpanel-Setup";
static const char* AP_PASSWORD = "flipflatpanel";
static const char* DEVICE_NAME = APP_HOSTNAME;
static constexpr uint8_t AP_CHANNEL = 1;
static constexpr uint8_t AP_MAX_CLIENTS = 4;
static constexpr int NETWORK_SCAN_MAX_RESULTS = 16;
static constexpr unsigned long NETWORK_SCAN_CACHE_MS = 300000;
static constexpr uint32_t NETWORK_SCAN_MAX_MS_PER_CHAN = 45;
static constexpr unsigned long WIFI_CHECK_INTERVAL_MS = 5000;
static constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 10000;
static constexpr unsigned long WIFI_AP_FALLBACK_MS = 45000;
static constexpr wifi_power_t WIFI_STA_TX_POWER = WIFI_POWER_11dBm;
static constexpr wifi_power_t WIFI_AP_SCAN_TX_POWER = WIFI_POWER_8_5dBm;
static IPAddress apIP(192, 168, 4, 1);
static String currentApSsid;
static String networkScanJson = "[]";
static String networkScanOptions;
static String networkScanError = "";
static String configuredSsid;
static String configuredPassword;
static bool networkScanRunning = false;
static bool networkScanRequested = false;
static bool networkScanForceRefresh = false;
static unsigned long networkScanRequestedMs = 0;
static unsigned long lastNetworkScanMs = 0;
static unsigned long lastWiFiCheckMs = 0;
static unsigned long wifiDisconnectedSinceMs = 0;
static unsigned long lastReconnectAttemptMs = 0;
static bool stationWasConnected = false;

static void setWiFiTxPower(wifi_power_t power) {
    WiFi.setTxPower(power);
}

static bool isAccessPointActive() {
    return WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA;
}

static void redirectToSetup(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(302, "text/plain", "");
    response->addHeader("Location", "/setup?portal=1");
    response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    request->send(response);
}

static bool connectStation(const String& ssid, const String& password, uint32_t timeoutMs) {
    if (ssid.isEmpty()) return false;

    const bool keepAccessPoint = isAccessPointActive();
    WiFi.setHostname(DEVICE_NAME);
    WiFi.mode(keepAccessPoint ? WIFI_AP_STA : WIFI_STA);
    setWiFiTxPower(WIFI_STA_TX_POWER);
    WiFi.disconnect(false, false);
    delay(150);

    Monitor::print("[WiFi] Teste Verbindung zu SSID: ");
    Monitor::println(ssid);
    WiFi.begin(ssid.c_str(), password.c_str());

    const unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Monitor::print("[WiFi] Verbindung erfolgreich, IP: ");
        Monitor::println(WiFi.localIP());
        return true;
    }

    Monitor::println("[WiFi] Verbindungstest fehlgeschlagen");
    if (keepAccessPoint) WiFi.mode(WIFI_AP_STA);
    return false;
}

static void stopAccessPointAfterResponse() {
    if (!isAccessPointActive()) return;
    delay(2500);
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    currentApSsid = "";
    Monitor::println("[WiFi] Setup-Hotspot nach erfolgreicher Verbindung beendet");
}

static String htmlEscape(String s) {
    s.replace("&", "&amp;");
    s.replace("<", "&lt;");
    s.replace(">", "&gt;");
    s.replace("\"", "&quot;");
    return s;
}

static String jsonEscape(String s) {
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    s.replace("\n", "\\n");
    s.replace("\r", "\\r");
    s.replace("\t", "\\t");
    return s;
}

static void updateNetworkScanCacheFromResults(int count) {
    if (count <= 0 && networkScanJson != "[]") {
        networkScanError = count == 0 ? "no_new_networks_found" : "scan_failed";
        networkScanRunning = false;
        WiFi.scanDelete();
        Monitor::println("[WiFi] Scan ohne neue Ergebnisse, vorhandene Liste bleibt erhalten");
        return;
    }

    String response = "[";
    String options;
    const int shownCount = min(count, NETWORK_SCAN_MAX_RESULTS);
    for (int i = 0; i < shownCount; ++i) {
        if (i > 0) response += ",";
        const String escapedSsid = htmlEscape(WiFi.SSID(i));
        response += "{\"ssid\":\"";
        response += jsonEscape(WiFi.SSID(i));
        response += "\",\"rssi\":";
        response += String(WiFi.RSSI(i));
        response += "}";

        options += "<option value=\"";
        options += escapedSsid;
        options += "\">";
        options += escapedSsid;
        options += " (";
        options += String(WiFi.RSSI(i));
        options += " dBm)</option>";
    }
    response += "]";

    networkScanJson = response;
    networkScanOptions = options;
    networkScanError = "";
    lastNetworkScanMs = millis();
    networkScanRunning = false;
    WiFi.scanDelete();

    Monitor::print("[WiFi] Scan abgeschlossen: ");
    Monitor::print(count);
    Monitor::print(" Netzwerke, angezeigt: ");
    Monitor::println(shownCount);
}

static void primeNetworkScanCache() {
    WiFi.scanDelete();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(500);

    Monitor::println("[WiFi] Initialer Scan vor AP/STA Start");
    const int result = WiFi.scanNetworks();
    if (result >= 0) {
        updateNetworkScanCacheFromResults(result);
        return;
    }

    networkScanError = "initial_scan_failed";
    Monitor::print("[WiFi] Initialer Scan fehlgeschlagen: ");
    Monitor::println(result);
    WiFi.scanDelete();
}

static void pollNetworkScan() {
    if (!networkScanRunning) return;

    const int result = WiFi.scanComplete();
    if (result >= 0) {
        updateNetworkScanCacheFromResults(result);
    } else if (result == -2) {
        networkScanRunning = false;
        networkScanError = "scan_failed";
        Monitor::println("[WiFi] Scan fehlgeschlagen");
    }
}

static void requestNetworkScan(bool force) {
    networkScanRequested = true;
    networkScanForceRefresh = networkScanForceRefresh || force;
    networkScanRequestedMs = millis();
    networkScanError = "";
}

static void handleNetworkScan() {
    pollNetworkScan();
    if (networkScanRunning || !networkScanRequested) return;

    const bool cacheFresh = (millis() - lastNetworkScanMs) < NETWORK_SCAN_CACHE_MS;
    if (!networkScanForceRefresh && cacheFresh && networkScanJson != "[]") {
        networkScanRequested = false;
        return;
    }

    WiFi.scanDelete();
    setWiFiTxPower(WIFI_AP_SCAN_TX_POWER);
    const int result = WiFi.scanNetworks(true, true, true, NETWORK_SCAN_MAX_MS_PER_CHAN);
    if (result == -1) {
        networkScanRunning = true;
        networkScanRequested = false;
        networkScanForceRefresh = false;
        networkScanError = "";
        Monitor::println("[WiFi] Asynchroner Scan gestartet");
    } else if (result >= 0) {
        networkScanRequested = false;
        networkScanForceRefresh = false;
        updateNetworkScanCacheFromResults(result);
    } else {
        networkScanRequested = false;
        networkScanForceRefresh = false;
        networkScanRunning = false;
        networkScanError = "scan_start_failed";
        Monitor::print("[WiFi] Scan konnte nicht gestartet werden: ");
        Monitor::println(result);
    }
}

static String buildSetupPage(const String& savedSsid, const String& networkOptions, bool networksPreloaded) {
    String html;
    html.reserve(28000 + savedSsid.length() + networkOptions.length());
    html += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FlipFlatpanel Setup</title>
<style>
:root{color-scheme:dark;--bg:#101114;--panel:#1b1d22;--panel2:#24272e;--line:#383d47;--text:#f2f2f2;--muted:#a9b0bd;--accent:#52b7ff;--warn:#ffbf66}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:Arial,Helvetica,sans-serif;line-height:1.4}
header{padding:14px 20px;border-bottom:1px solid var(--line);background:#15171b;position:sticky;top:0;z-index:2}
.header-inner{display:grid;grid-template-columns:minmax(0,1fr) minmax(280px,420px);gap:16px;align-items:start}
.header-left{display:grid;grid-template-columns:minmax(0,1fr) 160px;gap:12px;align-items:start}
.header-status{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:6px}
.header-system{display:grid;gap:7px}
h1{font-size:1.35rem;margin:0 0 6px}h2{font-size:1rem;margin:0 0 12px}.meta{color:var(--muted);font-size:.92rem}
main{max-width:1180px;margin:0 auto;padding:18px;display:grid;grid-template-columns:repeat(auto-fit,minmax(310px,1fr));gap:14px}
section{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:15px;min-width:0}
details.network-panel,details.fold-panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:0;grid-column:1/-1}
details.network-panel summary,details.fold-panel summary{cursor:pointer;padding:15px;font-weight:700;list-style:none}
details.network-panel summary::-webkit-details-marker,details.fold-panel summary::-webkit-details-marker{display:none}.network-body,.fold-body{padding:0 15px 15px}
body.network-active .control-section{opacity:.35;filter:grayscale(1);pointer-events:none}
.wide{grid-column:1/-1}.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.stack{display:grid;gap:10px}
button,input,select{font:inherit;border-radius:7px;border:1px solid var(--line);background:var(--panel2);color:var(--text);padding:10px;min-height:40px}
button{cursor:pointer;min-width:108px}button.primary{background:#0e5e91;border-color:#277eb6}button.warn{background:#704719;border-color:#a66f24}
button:disabled{opacity:.55;cursor:not-allowed}input,select{width:100%}input[type=range]{padding:0;min-height:32px}
label{display:grid;gap:5px;color:var(--muted);font-size:.86rem}.inline{display:flex;align-items:center;gap:8px}.inline input{width:auto;min-height:auto}.value{font-size:1.4rem;font-weight:700}.tiny{font-size:.82rem;color:var(--muted)}
.kv{display:grid;grid-template-columns:1fr auto;gap:8px 12px;align-items:center}.kv span:nth-child(odd){color:var(--muted)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:10px}.tile{background:#15171b;border:1px solid var(--line);border-radius:8px;padding:10px;min-height:72px}
.pill{display:inline-flex;align-items:center;gap:6px;border:1px solid var(--line);border-radius:999px;padding:4px 9px;color:var(--muted)}
.logbox{background:#0d0f12;border:1px solid var(--line);border-radius:8px;padding:12px;min-height:220px;max-height:420px;overflow:auto;white-space:pre-wrap;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.82rem;color:#d6dde8}
.status-list{display:grid;gap:8px}.status-row{display:grid;grid-template-columns:auto 1fr auto;gap:10px;align-items:center;background:#15171b;border:1px solid var(--line);border-radius:8px;padding:9px 10px}
.header-status .status-row{grid-template-columns:auto 1fr;padding:6px 8px;gap:7px;background:#101216}.header-status .status-row span{display:none}.header-status .status-row strong{font-size:.78rem;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.dot{width:11px;height:11px;border-radius:50%;background:var(--muted)}.dot.ok{background:#38d878}.dot.bad{background:#ff5e5e}.dot.pending{background:#ffbf66}
.status-row strong{font-size:.95rem}.status-row span:last-child{color:var(--muted);font-size:.88rem;text-align:right}
.status-legend{display:flex;gap:10px;justify-content:flex-end;flex-wrap:wrap;color:var(--muted);font-size:.76rem}
.legend-item{display:inline-flex;gap:5px;align-items:center}.legend-item .dot{width:8px;height:8px}
.language-select{max-width:280px}.header-language{margin-left:auto}.header-language label{gap:4px}.header-language select{min-height:34px;padding:6px 8px}
.ok{color:#7bd88f}.bad{color:#ff8c8c}.pending{color:var(--warn)}a{color:var(--accent)}
@media(max-width:760px){.header-inner,.header-left{grid-template-columns:1fr}.header-status{grid-template-columns:repeat(3,minmax(0,1fr))}.header-language{margin-left:0}}
</style>
</head>
<body>
<header>
  <div class="header-inner">
    <div class="header-left">
      <div>
        <h1 data-i18n="pageTitle">FlipFlatpanel Setup</h1>
        <div class="meta">IP: )rawliteral";
    html += getIPAddress();
    html += R"rawliteral( · <span data-i18n="setupHotspot">Setup-Hotspot</span>: )rawliteral";
    html += htmlEscape(currentApSsid.isEmpty() ? String(AP_SSID) : currentApSsid);
    html += R"rawliteral( · <span data-i18n="apPassword">AP-Passwort</span>: )rawliteral";
    html += AP_PASSWORD;
    html += R"rawliteral( · OTA: )rawliteral";
    html += APP_HOSTNAME;
    html += R"rawliteral(.local:3232</div>
      </div>
      <div class="header-language">
        <label><span data-i18n="language">Sprache</span>
          <select id="languageSelect" onchange="setLanguage(this.value)">
            <option value="de">Deutsch</option>
            <option value="en">English</option>
          </select>
        </label>
      </div>
    </div>
    <div class="header-system">
      <div class="header-status" title="Systemstatus">
        <div class="status-row"><i id="sysAlpacaDot" class="dot pending"></i><strong>Alpaca</strong><span id="sysAlpaca">Prüfe...</span></div>
        <div class="status-row"><i id="sysNetworkDot" class="dot pending"></i><strong data-i18n="networkShort">Netzwerk</strong><span id="sysNetwork">Prüfe...</span></div>
        <div class="status-row"><i id="sysBmeDot" class="dot pending"></i><strong>BME</strong><span id="sysBme">Prüfe...</span></div>
        <div class="status-row"><i id="sysOtaDot" class="dot pending"></i><strong>OTA</strong><span id="sysOta">Prüfe...</span></div>
        <div class="status-row"><i id="sysServoDot" class="dot ok"></i><strong>Servo</strong><span data-i18n="io4Active">IO4 aktiv</span></div>
        <div class="status-row"><i id="sysPanelDot" class="dot ok"></i><strong>Panel</strong><span data-i18n="io13Active">IO13 aktiv</span></div>
        <div class="status-row"><i id="sysHeaterDot" class="dot pending"></i><strong data-i18n="heater">Heizung</strong><span id="sysHeater">Prüfe...</span></div>
      </div>
      <div class="status-legend">
        <span class="legend-item"><i class="dot ok"></i><span data-i18n="legendOk">OK</span></span>
        <span class="legend-item"><i class="dot pending"></i><span data-i18n="legendPending">Hinweis</span></span>
        <span class="legend-item"><i class="dot bad"></i><span data-i18n="legendBad">Fehler</span></span>
      </div>
    </div>
  </div>
</header>
<main>
  <section class="wide control-section">
    <h2 data-i18n="status">Status</h2>
    <div class="row" style="margin-bottom:12px">
      <button type="button" onclick="refresh()" data-i18n="refresh">Aktualisieren</button>
    </div>
    <div class="grid">
      <div class="tile"><div class="tiny" data-i18n="cover">Deckel</div><div class="value" id="coverState">-</div></div>
      <div class="tile"><div class="tiny" data-i18n="light">Licht</div><div class="value" id="lightState">-</div></div>
      <div class="tile"><div class="tiny" data-i18n="heater">Heizung</div><div class="value" id="heaterState">-</div></div>
      <div class="tile"><div class="tiny">BME280</div><div class="value" id="bmeState">-</div></div>
    </div>
  </section>

  <details id="networkDetails" class="network-panel">
    <summary data-i18n="network">Netzwerk</summary>
    <div class="network-body">
    <form class="stack" method="POST" action="/setup/save">
      <label><span data-i18n="foundNetworks">Gefundene WLANs</span>
        <select id="networks" onchange="document.getElementById('ssid').value=this.value">
          <option value="">)rawliteral";
    html += networksPreloaded ? "WLAN auswählen" : "SSID manuell eingeben oder Scan starten";
    html += R"rawliteral(</option>)rawliteral";
    html += networkOptions;
    html += R"rawliteral(
        </select>
      </label>
      <div class="tiny" data-i18n="reloadToScan">Bitte Seite neu laden, um erneut nach WLANs zu scannen.</div>
      <label>SSID
        <input id="ssid" name="ssid" autocomplete="username" value=")rawliteral";
    html += savedSsid;
    html += R"rawliteral(">
      </label>
      <label><span data-i18n="password">Passwort</span>
        <input name="password" type="password" autocomplete="current-password" value="">
      </label>
      <div class="row">
        <button class="primary" type="submit" data-i18n="save">Speichern</button>
        <button class="warn" type="submit" formaction="/setup/reset" data-i18n="resetWifi">WLAN zurücksetzen</button>
      </div>
      <div class="tiny" data-i18n="wifiSaveHint">Beim Speichern bleibt der Hotspot aktiv, bis die WLAN-Verbindung erfolgreich getestet wurde.</div>
    </form>
    </div>
  </details>

  <details id="logDetails" class="wide control-section">
    <summary data-i18n="monitorLog">Fehler / Monitor-Log</summary>
    <div class="row" style="margin:12px 0">
      <button type="button" onclick="loadLogs()" data-i18n="refresh">Aktualisieren</button>
      <button type="button" onclick="clearLogs()" data-i18n="clear">Leeren</button>
    </div>
    <pre id="monitorLog" class="logbox" data-i18n="logLoading">Log wird geladen...</pre>
  </details>

  <section class="control-section">
    <h2 data-i18n="cover">Deckel</h2>
    <div class="row">
      <button class="primary" onclick="put('/api/v1/covercalibrator/0/opencover')" data-i18n="open">Öffnen</button>
      <button class="primary" onclick="put('/api/v1/covercalibrator/0/closecover')" data-i18n="close">Schließen</button>
    </div>
    <div class="kv" style="margin-top:14px">
      <span>Servo Pin</span><strong>IO4</strong>
      <span data-i18n="status">Status</span><strong id="coverState2">-</strong>
      <span data-i18n="position">Position</span><strong id="mappedPos">-</strong>
    </div>
  </section>

  <section class="control-section">
    <h2 data-i18n="lightPanel">Lichtpanel</h2>
    <div class="row">
      <button class="primary" onclick="panelOn()" data-i18n="on">Ein</button>
      <button onclick="put('/api/v1/covercalibrator/0/calibratoroff')" data-i18n="off">Aus</button>
    </div>
    <label style="margin-top:12px"><span data-i18n="brightness">Helligkeit</span> <span id="brightnessText">0</span>
      <input id="brightness" type="range" min="0" max="255" value="0" oninput="brightnessText.textContent=this.value" onchange="setBrightness(this.value)">
    </label>
    <div class="tiny" data-i18n="panelPwmHint">Panel PWM liegt auf IO13.</div>
  </section>

  <section class="control-section">
    <h2 data-i18n="environment">Umgebung</h2>
    <div class="grid">
      <div class="tile"><div class="tiny" data-i18n="temperature">Temperatur</div><div class="value" id="temp">-</div></div>
      <div class="tile"><div class="tiny" data-i18n="humidity">Luftfeuchte</div><div class="value" id="hum">-</div></div>
      <div class="tile"><div class="tiny" data-i18n="pressure">Luftdruck</div><div class="value" id="pres">-</div></div>
      <div class="tile"><div class="tiny" data-i18n="dewPoint">Taupunkt</div><div class="value" id="dew">-</div></div>
    </div>
  </section>

  <section class="control-section">
    <h2 data-i18n="heater">Heizung</h2>
    <div class="row">
      <button class="primary" onclick="heaterAuto(true)" data-i18n="dewAuto">Taupunkt Auto</button>
      <button onclick="heaterAuto(false)" data-i18n="autoOff">Auto Aus</button>
      <button onclick="heaterManual(true)" data-i18n="manualOn">Manuell Ein</button>
      <button onclick="heaterManual(false)" data-i18n="manualOff">Manuell Aus</button>
    </div>
    <div class="kv" style="margin-top:14px">
      <span data-i18n="output">Ausgang</span><strong id="heaterOutput">-</strong>
      <span data-i18n="mode">Modus</span><strong id="heaterMode">-</strong>
      <span data-i18n="autoActive">Auto aktiv</span><strong id="heaterAuto">-</strong>
    </div>
  </section>

  <details id="positionDetails" class="fold-panel">
    <summary data-i18n="positionCalibration">Positionskalibrierung</summary>
    <div class="fold-body">
    <div class="grid" style="margin-bottom:12px">
      <div class="tile"><div class="tiny" data-i18n="servoPulse">Servo Puls</div><div class="value" id="servoPulseTile">-</div></div>
      <div class="tile"><div class="tiny" data-i18n="adcRaw">ADC roh</div><div class="value" id="rawPos">-</div></div>
      <div class="tile"><div class="tiny" data-i18n="adcFiltered">ADC gefiltert</div><div class="value" id="filteredPos">-</div></div>
      <div class="tile"><div class="tiny" data-i18n="position">Position</div><div class="value" id="mappedPos2">-</div></div>
    </div>
    <label><span data-i18n="manualServo">Servo manuell fahren</span> <span id="servoPulseText">-</span>
      <input id="servoPulse" type="range" min="500" max="2500" step="10" value="1500" oninput="servoPreview(this.value)" onchange="servoSet(this.value)">
    </label>
    <label class="inline" style="margin-top:10px">
      <input id="servoSmooth" type="checkbox" onchange="servoSmoothSet(this.checked)">
      <span data-i18n="servoSmooth">Servo sanft anfahren/abfahren</span>
    </label>
    <label style="margin-top:10px"><span data-i18n="maxSpeed">Maximalgeschwindigkeit</span> <span id="servoSpeedText">-</span>
      <input id="servoSpeed" type="range" min="100" max="3000" step="50" value="1000" oninput="servoSpeedPreview(this.value)" onchange="servoSpeedSet(this.value)">
    </label>
    <div class="row" style="margin:10px 0 14px">
      <button onclick="servoStep(-100)">-100 us</button>
      <button onclick="servoStep(-10)">-10 us</button>
      <button onclick="servoStep(10)">+10 us</button>
      <button onclick="servoStep(100)">+100 us</button>
      <button onclick="servoSet(servoOpenUs)" data-i18n="openPulse">Offen-Puls</button>
      <button onclick="servoSet(servoCloseUs)" data-i18n="closedPulse">Geschlossen-Puls</button>
    </div>
    <div class="row">
      <button onclick="put('/api/v1/covercalibrator/0/opencover')" data-i18n="openCover">Deckel öffnen</button>
      <button class="primary" onclick="put('/api/v1/flatpanel/calibrate/open')" data-i18n="saveAsOpen">Aktuell als Offen speichern</button>
      <button onclick="put('/api/v1/covercalibrator/0/closecover')" data-i18n="closeCover">Deckel schließen</button>
      <button class="primary" onclick="put('/api/v1/flatpanel/calibrate/closed')" data-i18n="saveAsClosed">Aktuell als Geschlossen speichern</button>
      <button class="warn" onclick="put('/api/v1/flatpanel/calibrate/reset')" data-i18n="resetCalibration">Kalibrierung zurücksetzen</button>
    </div>
    <div class="grid" style="margin-top:12px">
      <div class="tile"><div class="tiny" data-i18n="openRaw">Offen RAW</div><div class="value" id="openRaw">-</div></div>
      <div class="tile"><div class="tiny" data-i18n="closedRaw">Geschlossen RAW</div><div class="value" id="closedRaw">-</div></div>
      <div class="tile"><div class="tiny" data-i18n="openServo">Offen Servo</div><div class="value" id="openServo">-</div></div>
      <div class="tile"><div class="tiny" data-i18n="closedServo">Geschlossen Servo</div><div class="value" id="closedServo">-</div></div>
    </div>
    <div class="tiny" style="margin-top:10px" data-i18n="calibrationHint">Ablauf: mit dem manuellen Servo-Regler exakt auf Offen fahren, Offen speichern, dann exakt auf Geschlossen fahren und Geschlossen speichern. Dabei werden Servo-Puls und ADC-Wert gemeinsam übernommen.</div>
    </div>
  </details>

  <section class="wide control-section">
    <h2 data-i18n="system">System</h2>
    <div class="row">
      <a class="pill" href="/management/v1/configureddevices" data-i18n="alpacaDevices">Alpaca Geräte</a>
      <a class="pill" href="/api/v1/flatpanel/environment">JSON Status</a>
      <a class="pill" href="/monitor/log">Monitor Log</a>
    </div>
    <div class="tiny" style="margin-top:10px" data-i18n="otaHint">OTA-Upload über PlatformIO: Environment esp32s2_ota, Hostname flipflatpanel.local oder die aktuelle IP.</div>
  </section>
</main>
<script>
const setupTranslations={
  de:{
    pageTitle:'FlipFlatpanel Setup',setupHotspot:'Setup-Hotspot',apPassword:'AP-Passwort',networkShort:'Netzwerk',heater:'Heizung',
    language:'Sprache',languageSelect:'Anzeigesprache',status:'Status',cover:'Deckel',light:'Licht',network:'Netzwerk',
    foundNetworks:'Gefundene WLANs',selectNetwork:'WLAN auswählen',manualOrScan:'SSID manuell eingeben oder Scan starten',
    reloadToScan:'Bitte Seite neu laden, um erneut nach WLANs zu scannen.',password:'Passwort',save:'Speichern',resetWifi:'WLAN zurücksetzen',
    wifiSaveHint:'Beim Speichern bleibt der Hotspot aktiv, bis die WLAN-Verbindung erfolgreich getestet wurde.',
    monitorLog:'Fehler / Monitor-Log',refresh:'Aktualisieren',clear:'Leeren',logLoading:'Log wird geladen...',logEmpty:'Noch keine Meldungen.',logUnavailable:'Log nicht erreichbar',
    open:'Öffnen',close:'Schließen',position:'Position',lightPanel:'Lichtpanel',on:'Ein',off:'Aus',brightness:'Helligkeit',panelPwmHint:'Panel PWM liegt auf IO13.',
    environment:'Umgebung',temperature:'Temperatur',humidity:'Luftfeuchte',pressure:'Luftdruck',dewPoint:'Taupunkt',
    dewAuto:'Taupunkt Auto',autoOff:'Auto Aus',manualOn:'Manuell Ein',manualOff:'Manuell Aus',output:'Ausgang',mode:'Modus',autoActive:'Auto aktiv',
    positionCalibration:'Positionskalibrierung',servoPulse:'Servo Puls',adcRaw:'ADC roh',adcFiltered:'ADC gefiltert',manualServo:'Servo manuell fahren',
    servoSmooth:'Servo sanft anfahren/abfahren',maxSpeed:'Maximalgeschwindigkeit',openPulse:'Offen-Puls',closedPulse:'Geschlossen-Puls',
    openCover:'Deckel öffnen',closeCover:'Deckel schließen',saveAsOpen:'Aktuell als Offen speichern',saveAsClosed:'Aktuell als Geschlossen speichern',resetCalibration:'Kalibrierung zurücksetzen',
    openRaw:'Offen RAW',closedRaw:'Geschlossen RAW',openServo:'Offen Servo',closedServo:'Geschlossen Servo',
    calibrationHint:'Ablauf: mit dem manuellen Servo-Regler exakt auf Offen fahren, Offen speichern, dann exakt auf Geschlossen fahren und Geschlossen speichern. Dabei werden Servo-Puls und ADC-Wert gemeinsam übernommen.',
    system:'System',alpacaDevices:'Alpaca Geräte',otaHint:'OTA-Upload über PlatformIO: Environment esp32s2_ota, Hostname flipflatpanel.local oder die aktuelle IP.',
    check:'Prüfe...',httpOk:'HTTP OK',wlan:'WLAN',setupHotspotState:'Setup-Hotspot',noConnection:'Keine Verbindung',missing:'Fehlt',ready:'Bereit',notReady:'Nicht bereit',io4Active:'IO4 aktiv',io13Active:'IO13 aktiv',legendOk:'OK',legendPending:'Hinweis',legendBad:'Fehler',
    manualOnState:'Manuell ein',manualOffState:'Manuell aus',dewAutomation:'Taupunkt-Automatik',automationOff:'Automatik aus',unreachable:'Nicht erreichbar',statusUnavailable:'Status nicht abrufbar',
    coverOpen:'Offen',coverClosed:'Geschlossen',coverMoving:'Bewegt',coverUnknown:'Unbekannt',coverError:'Fehler',
    isOn:'AN',isOff:'AUS',manual:'Manuell',scanRunning:'Suche läuft...',scanFailedManual:'Scan fehlgeschlagen: SSID manuell eintragen',noNetworks:'Keine WLANs gefunden',scanUnavailableManual:'Scan nicht erreichbar: SSID manuell eintragen',scanInterrupted:'Scan läuft, Verbindung kurz unterbrochen...'
  },
  en:{
    pageTitle:'FlipFlatpanel Setup',setupHotspot:'Setup hotspot',apPassword:'AP password',networkShort:'Network',heater:'Heater',
    language:'Language',languageSelect:'Display language',status:'Status',cover:'Cover',light:'Light',network:'Network',
    foundNetworks:'Found Wi-Fi networks',selectNetwork:'Select Wi-Fi',manualOrScan:'Enter SSID manually or reload to scan',
    reloadToScan:'Reload this page to scan for Wi-Fi networks again.',password:'Password',save:'Save',resetWifi:'Reset Wi-Fi',
    wifiSaveHint:'When saving, the setup hotspot remains active until the Wi-Fi connection has been tested successfully.',
    monitorLog:'Errors / Monitor log',refresh:'Refresh',clear:'Clear',logLoading:'Loading log...',logEmpty:'No messages yet.',logUnavailable:'Log unavailable',
    open:'Open',close:'Close',position:'Position',lightPanel:'Light panel',on:'On',off:'Off',brightness:'Brightness',panelPwmHint:'Panel PWM is on IO13.',
    environment:'Environment',temperature:'Temperature',humidity:'Humidity',pressure:'Pressure',dewPoint:'Dew point',
    dewAuto:'Dew point auto',autoOff:'Auto off',manualOn:'Manual on',manualOff:'Manual off',output:'Output',mode:'Mode',autoActive:'Auto active',
    positionCalibration:'Position calibration',servoPulse:'Servo pulse',adcRaw:'ADC raw',adcFiltered:'ADC filtered',manualServo:'Manual servo control',
    servoSmooth:'Smooth servo start/stop',maxSpeed:'Maximum speed',openPulse:'Open pulse',closedPulse:'Closed pulse',
    openCover:'Open cover',closeCover:'Close cover',saveAsOpen:'Save current as open',saveAsClosed:'Save current as closed',resetCalibration:'Reset calibration',
    openRaw:'Open RAW',closedRaw:'Closed RAW',openServo:'Open servo',closedServo:'Closed servo',
    calibrationHint:'Workflow: use the manual servo slider to move exactly to the open position, save open, then move exactly to the closed position and save closed. Servo pulse and ADC value are stored together.',
    system:'System',alpacaDevices:'Alpaca devices',otaHint:'OTA upload via PlatformIO: environment esp32s2_ota, hostname flipflatpanel.local or the current IP.',
    check:'Checking...',httpOk:'HTTP OK',wlan:'Wi-Fi',setupHotspotState:'Setup hotspot',noConnection:'No connection',missing:'Missing',ready:'Ready',notReady:'Not ready',io4Active:'IO4 active',io13Active:'IO13 active',legendOk:'OK',legendPending:'Notice',legendBad:'Error',
    manualOnState:'Manual on',manualOffState:'Manual off',dewAutomation:'Dew point automation',automationOff:'Automation off',unreachable:'Unreachable',statusUnavailable:'Status unavailable',
    coverOpen:'Open',coverClosed:'Closed',coverMoving:'Moving',coverUnknown:'Unknown',coverError:'Error',
    isOn:'ON',isOff:'OFF',manual:'Manual',scanRunning:'Scanning...',scanFailedManual:'Scan failed: enter SSID manually',noNetworks:'No Wi-Fi networks found',scanUnavailableManual:'Scan unavailable: enter SSID manually',scanInterrupted:'Scan running, connection briefly interrupted...'
  }
};
let currentLang='de';
try{currentLang=localStorage.getItem('flipflatpanel_lang') || 'de'}catch(e){}
function tr(key){return (setupTranslations[currentLang]&&setupTranslations[currentLang][key]) || setupTranslations.de[key] || key}
function applyLanguage(){
  document.documentElement.lang=currentLang;
  const langSelect=document.getElementById('languageSelect');
  if(langSelect) langSelect.value=currentLang;
  document.querySelectorAll('[data-i18n]').forEach(function(el){el.textContent=tr(el.dataset.i18n)});
  const networks=document.getElementById('networks');
  if(networks && networks.options.length && networks.options[0].value===''){
    networks.options[0].textContent=networksPreloaded ? tr('selectNetwork') : tr('manualOrScan');
  }
}
function setLanguage(lang){
  currentLang=(lang==='en')?'en':'de';
  try{localStorage.setItem('flipflatpanel_lang',currentLang)}catch(e){}
  applyLanguage();
  refresh();
}
async function put(url, body=''){
  try{
    await fetch(url,{method:'PUT',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});
    setTimeout(refresh,150);
  }catch(e){}
}
function panelOn(){
  const slider=document.getElementById('brightness');
  slider.value=255;
  text('brightnessText','255');
  put('/api/v1/covercalibrator/0/calibratoron','Brightness=255');
}
function setBrightness(v){put('/api/v1/covercalibrator/0/brightness','Brightness='+encodeURIComponent(v))}
function heaterAuto(enabled){put('/api/v1/flatpanel/heaterenabled','Enabled='+(enabled?'true':'false'))}
function heaterManual(on){put('/api/v1/flatpanel/heatermanual','On='+(on?'true':'false'))}
let servoPulseUs=1500, servoMinUs=500, servoMaxUs=2500, servoOpenUs=2500, servoCloseUs=500, servoSpeed=1000, servoSpeedMin=100, servoSpeedMax=3000;
function servoPreview(v){text('servoPulseText',v+' us'); text('servoPulseTile',v+' us')}
function servoSpeedPreview(v){text('servoSpeedText',v+' us/s')}
function servoSet(v){
  servoPulseUs=Math.max(servoMinUs,Math.min(servoMaxUs,Number(v)||servoPulseUs));
  const slider=document.getElementById('servoPulse'); slider.value=servoPulseUs; servoPreview(servoPulseUs);
  put('/api/v1/flatpanel/servo','PulseUs='+encodeURIComponent(servoPulseUs));
}
function servoStep(delta){servoSet(servoPulseUs+delta)}
function servoSmoothSet(enabled){put('/api/v1/flatpanel/servosmooth','Enabled='+(enabled?'true':'false'))}
function servoSpeedSet(v){
  servoSpeed=Math.max(servoSpeedMin,Math.min(servoSpeedMax,Number(v)||servoSpeed));
  const slider=document.getElementById('servoSpeed'); slider.value=servoSpeed; servoSpeedPreview(servoSpeed);
  put('/api/v1/flatpanel/servospeed','MaxSpeedUsPerSec='+encodeURIComponent(servoSpeed));
}
function fmt(v,d,u){return Number.isFinite(v)?Number(v).toFixed(d)+u:'-'}
function valueOr(v,fallback){return (v===undefined || v===null) ? fallback : v}
function text(id,value){const el=document.getElementById(id); if(el) el.textContent=value}
function coverLabel(value){
  const normalized=String(value||'').toLowerCase();
  if(normalized==='offen' || normalized==='open') return tr('coverOpen');
  if(normalized==='geschlossen' || normalized==='closed') return tr('coverClosed');
  if(normalized==='moving' || normalized==='bewegt') return tr('coverMoving');
  if(normalized==='error' || normalized==='fehler') return tr('coverError');
  return tr('coverUnknown');
}
function setSystem(id,state,label){
  const dot=document.getElementById(id+'Dot');
  if(dot) dot.className='dot '+state;
  text(id,label);
}
function updateSystemStatus(d){
  setSystem('sysAlpaca','ok',tr('httpOk'));
  if(d.networkConnected){
    const rssi=Number.isFinite(d.networkRssi)?' · '+d.networkRssi+' dBm':'';
    setSystem('sysNetwork','ok',(d.networkSsid||tr('wlan'))+' · '+(d.networkIp||'-')+rssi);
  }else if(d.accessPointActive){
    setSystem('sysNetwork','pending',tr('setupHotspotState')+' · '+(d.accessPointIp||'192.168.4.1'));
  }else{
    setSystem('sysNetwork','bad',tr('noConnection'));
  }
  setSystem('sysBme',d.bmeAvailable?'ok':'bad',d.bmeAvailable?'OK':tr('missing'));
  setSystem('sysOta',d.otaReady?'ok':'bad',d.otaReady?tr('ready'):tr('notReady'));
  if(d.heaterManualMode){
    setSystem('sysHeater',d.heaterOn?'ok':'pending',d.heaterOn?tr('manualOnState'):tr('manualOffState'));
  }else{
    setSystem('sysHeater',d.heaterEnabled?'ok':'pending',d.heaterEnabled?tr('dewAutomation'):tr('automationOff'));
  }
}
async function refresh(){
  if(document.body.classList.contains('network-active')) return;
  try{
    const r=await fetch('/api/v1/flatpanel/environment',{cache:'no-store'});
    const d=await r.json();
    updateSystemStatus(d);
    const coverText=coverLabel(d.coverStateText);
    text('coverState',coverText); text('coverState2',coverText);
    text('lightState',d.brightness>0?tr('isOn'):tr('isOff')); text('brightnessText',valueOr(d.brightness,0));
    const slider=document.getElementById('brightness'); if(document.activeElement!==slider) slider.value=valueOr(d.brightness,0);
    servoMinUs=valueOr(d.servoMinUs,servoMinUs); servoMaxUs=valueOr(d.servoMaxUs,servoMaxUs); servoOpenUs=valueOr(d.servoOpenUs,servoOpenUs); servoCloseUs=valueOr(d.servoCloseUs,servoCloseUs);
    servoPulseUs=valueOr(d.servoPulseUs,servoPulseUs);
    const servoSlider=document.getElementById('servoPulse');
    servoSlider.min=servoMinUs; servoSlider.max=servoMaxUs;
    if(document.activeElement!==servoSlider) servoSlider.value=servoPulseUs;
    servoPreview(servoSlider.value);
    const smooth=document.getElementById('servoSmooth'); if(document.activeElement!==smooth) smooth.checked=!!d.servoSmoothEnabled;
    servoSpeedMin=valueOr(d.servoSpeedMinUsPerSec,servoSpeedMin); servoSpeedMax=valueOr(d.servoSpeedMaxUsPerSec,servoSpeedMax); servoSpeed=valueOr(d.servoMaxSpeedUsPerSec,servoSpeed);
    const speedSlider=document.getElementById('servoSpeed');
    speedSlider.min=servoSpeedMin; speedSlider.max=servoSpeedMax;
    if(document.activeElement!==speedSlider) speedSlider.value=servoSpeed;
    servoSpeedPreview(speedSlider.value);
    text('heaterState',d.heaterOn?tr('isOn'):tr('isOff')); text('heaterOutput',d.heaterOn?tr('isOn'):tr('isOff'));
    text('heaterMode',d.heaterManualMode?tr('manual'):tr('dewPoint')); text('heaterAuto',d.heaterEnabled?tr('isOn'):tr('isOff'));
    text('bmeState',d.bmeAvailable?'OK':tr('missing'));
    text('temp',fmt(d.temperatureC,1,' °C')); text('hum',fmt(d.humidityPct,1,' %')); text('pres',fmt(d.pressureHpa,1,' hPa')); text('dew',fmt(d.dewPointC,1,' °C'));
    text('mappedPos',Number.isFinite(d.mappedPosition)?d.mappedPosition+' %':'-'); text('mappedPos2',Number.isFinite(d.mappedPosition)?d.mappedPosition+' %':'-'); text('rawPos',valueOr(d.rawPosition,'-')); text('filteredPos',valueOr(d.filteredPosition,'-'));
    text('openRaw',valueOr(d.calibrationOpenRaw,'-')); text('closedRaw',valueOr(d.calibrationClosedRaw,'-'));
    text('openServo',Number.isFinite(d.calibrationOpenServoUs)?d.calibrationOpenServoUs+' us':'-'); text('closedServo',Number.isFinite(d.calibrationClosedServoUs)?d.calibrationClosedServoUs+' us':'-');
  }catch(e){
    setSystem('sysAlpaca','bad',tr('unreachable'));
    setSystem('sysNetwork','bad',tr('statusUnavailable'));
    console.log(e)
  }
  const logPanel=document.getElementById('logDetails');
  if(logPanel && logPanel.open) loadLogs(false);
}
function updateNetworkMode(){
  const details=document.getElementById('networkDetails');
  document.body.classList.toggle('network-active',details && details.open);
}
async function loadLogs(scroll=true){
  try{
    const r=await fetch('/monitor/log?ts='+Date.now(),{cache:'no-store'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    const log=await r.text();
    const box=document.getElementById('monitorLog');
    box.textContent=log || tr('logEmpty');
    if(scroll) box.scrollTop=box.scrollHeight;
  }catch(e){
    text('monitorLog',tr('logUnavailable'));
  }
}
async function clearLogs(){
  await fetch('/monitor/log/clear',{method:'POST'});
  loadLogs();
}
async function loadNetworks(force=false, attempt=0){
  const select=document.getElementById('networks');
  select.innerHTML='<option value="">'+tr('scanRunning')+'</option>';
  try{
    const r=await fetch('/setup/networks'+(force?'?refresh=1':''),{cache:'no-store'});
    const payload=await r.json();
    const list=Array.isArray(payload)?payload:(payload.networks||[]);
    select.innerHTML='<option value="">'+tr('selectNetwork')+'</option>';
    for(const n of list){
      const opt=document.createElement('option');
      opt.value=n.ssid; opt.textContent=n.ssid+' ('+n.rssi+' dBm)';
      select.appendChild(opt);
    }
    if(payload.scanning || payload.requested){
      if(!list.length) select.innerHTML='<option value="">'+tr('scanRunning')+'</option>';
      setTimeout(function(){loadNetworks(false,attempt+1)},1200);
    }else if(payload.error && !list.length){
      select.innerHTML='<option value="">'+tr('scanFailedManual')+'</option>';
    }else if(!list.length){
      select.innerHTML='<option value="">'+tr('noNetworks')+'</option>';
    }
  }catch(e){
    if(attempt<12){
      select.innerHTML='<option value="">'+tr('scanInterrupted')+'</option>';
      setTimeout(function(){loadNetworks(false,attempt+1)},1500);
    }else{
      select.innerHTML='<option value="">'+tr('scanUnavailableManual')+'</option>';
    }
  }
}
const networksPreloaded=)rawliteral";
    html += networksPreloaded ? "true" : "false";
    html += R"rawliteral(;
const networkDetails=document.getElementById('networkDetails');
if(networkDetails) networkDetails.addEventListener('toggle',updateNetworkMode);
const logDetails=document.getElementById('logDetails');
if(logDetails) logDetails.addEventListener('toggle',function(){if(logDetails.open) loadLogs();});
applyLanguage();
updateNetworkMode();
</script>
</body>
</html>)rawliteral";
    return html;
}

static String buildSplitSetupPage(const String& savedSsid, const String& networkOptions, bool networksPreloaded) {
    String html;
    html.reserve(17000 + savedSsid.length() + networkOptions.length());
    html += R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FlipFlatpanel Setup</title>
<style>
:root{color-scheme:dark;--bg:#101114;--panel:#1b1d22;--panel2:#24272e;--line:#383d47;--text:#f2f2f2;--muted:#a9b0bd;--accent:#52b7ff;--warn:#ffbf66}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:Arial,Helvetica,sans-serif;line-height:1.4}
header{padding:14px 20px;border-bottom:1px solid var(--line);background:#15171b;position:sticky;top:0;z-index:2}
.header-inner{display:grid;grid-template-columns:minmax(0,1fr) minmax(280px,420px);gap:16px;align-items:start}.header-left{display:grid;grid-template-columns:minmax(0,1fr) 160px;gap:12px;align-items:start}.header-status{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:6px}.header-system{display:grid;gap:7px}
h1{font-size:1.35rem;margin:0 0 6px}h2{font-size:1rem;margin:0 0 12px}.meta{color:var(--muted);font-size:.92rem}
main{max-width:1180px;margin:0 auto;padding:18px;display:grid;grid-template-columns:repeat(auto-fit,minmax(310px,1fr));gap:14px}
section,details{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:15px;min-width:0}.wide{grid-column:1/-1}summary{cursor:pointer;font-weight:700;list-style:none}summary::-webkit-details-marker{display:none}
body.network-active .control-section{opacity:.35;filter:grayscale(1);pointer-events:none}.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.stack{display:grid;gap:10px}
button,input,select{font:inherit;border-radius:7px;border:1px solid var(--line);background:var(--panel2);color:var(--text);padding:10px;min-height:40px}button{cursor:pointer;min-width:108px}button.primary{background:#0e5e91;border-color:#277eb6}button.warn{background:#704719;border-color:#a66f24}input,select{width:100%}input[type=range]{padding:0;min-height:32px}
label{display:grid;gap:5px;color:var(--muted);font-size:.86rem}.inline{display:flex;align-items:center;gap:8px}.inline input{width:auto;min-height:auto}.value{font-size:1.4rem;font-weight:700}.tiny{font-size:.82rem;color:var(--muted)}
.kv{display:grid;grid-template-columns:1fr auto;gap:8px 12px;align-items:center}.kv span:nth-child(odd){color:var(--muted)}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:10px}.tile{background:#15171b;border:1px solid var(--line);border-radius:8px;padding:10px;min-height:72px}
.pill{display:inline-flex;align-items:center;gap:6px;border:1px solid var(--line);border-radius:999px;padding:4px 9px;color:var(--muted)}.logbox{background:#0d0f12;border:1px solid var(--line);border-radius:8px;padding:12px;min-height:220px;max-height:420px;overflow:auto;white-space:pre-wrap;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.82rem;color:#d6dde8}
.status-row{display:grid;grid-template-columns:auto 1fr auto;gap:10px;align-items:center;background:#15171b;border:1px solid var(--line);border-radius:8px;padding:9px 10px}.header-status .status-row{grid-template-columns:auto 1fr;padding:6px 8px;gap:7px;background:#101216}.header-status .status-row span{display:none}.header-status .status-row strong{font-size:.78rem;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.dot{width:11px;height:11px;border-radius:50%;background:var(--muted)}.dot.ok{background:#38d878}.dot.bad{background:#ff5e5e}.dot.pending{background:#ffbf66}.status-legend{display:flex;gap:10px;justify-content:flex-end;flex-wrap:wrap;color:var(--muted);font-size:.76rem}.legend-item{display:inline-flex;gap:5px;align-items:center}.legend-item .dot{width:8px;height:8px}.header-language{margin-left:auto}.header-language label{gap:4px}.header-language select{min-height:34px;padding:6px 8px}.ok{color:#7bd88f}.bad{color:#ff8c8c}.pending{color:var(--warn)}a{color:var(--accent)}
@media(max-width:760px){.header-inner,.header-left{grid-template-columns:1fr}.header-status{grid-template-columns:repeat(3,minmax(0,1fr))}.header-language{margin-left:0}}
</style>
</head>
<body data-networks-preloaded=")rawliteral";
    html += networksPreloaded ? "1" : "0";
    html += R"rawliteral(">
<header><div class="header-inner"><div class="header-left"><div><h1 data-i18n="pageTitle">FlipFlatpanel Setup</h1><div class="meta">IP: )rawliteral";
    html += getIPAddress();
    html += R"rawliteral( · <span data-i18n="setupHotspot">Setup-Hotspot</span>: )rawliteral";
    html += htmlEscape(currentApSsid.isEmpty() ? String(AP_SSID) : currentApSsid);
    html += R"rawliteral( · <span data-i18n="apPassword">AP-Passwort</span>: )rawliteral";
    html += AP_PASSWORD;
    html += R"rawliteral( · OTA: )rawliteral";
    html += APP_HOSTNAME;
    html += R"rawliteral(.local:3232</div></div><div class="header-language"><label><span data-i18n="language">Sprache</span><select id="languageSelect" onchange="setLanguage(this.value)"><option value="de">Deutsch</option><option value="en">English</option></select></label></div></div>
<div class="header-system"><div class="header-status"><div class="status-row"><i id="sysAlpacaDot" class="dot pending"></i><strong>Alpaca</strong><span id="sysAlpaca">Prüfe...</span></div><div class="status-row"><i id="sysNetworkDot" class="dot pending"></i><strong data-i18n="networkShort">Netzwerk</strong><span id="sysNetwork">Prüfe...</span></div><div class="status-row"><i id="sysBmeDot" class="dot pending"></i><strong>BME</strong><span id="sysBme">Prüfe...</span></div><div class="status-row"><i id="sysOtaDot" class="dot pending"></i><strong>OTA</strong><span id="sysOta">Prüfe...</span></div><div class="status-row"><i id="sysServoDot" class="dot ok"></i><strong>Servo</strong><span data-i18n="io4Active">IO4 aktiv</span></div><div class="status-row"><i id="sysPanelDot" class="dot ok"></i><strong>Panel</strong><span data-i18n="io13Active">IO13 aktiv</span></div><div class="status-row"><i id="sysHeaterDot" class="dot pending"></i><strong data-i18n="heater">Heizung</strong><span id="sysHeater">Prüfe...</span></div></div><div class="status-legend"><span class="legend-item"><i class="dot ok"></i><span data-i18n="legendOk">OK</span></span><span class="legend-item"><i class="dot pending"></i><span data-i18n="legendPending">Hinweis</span></span><span class="legend-item"><i class="dot bad"></i><span data-i18n="legendBad">Fehler</span></span></div></div></div></header>
<main>
<section class="wide control-section"><h2 data-i18n="status">Status</h2><div class="grid"><div class="tile"><div class="tiny" data-i18n="cover">Deckel</div><div class="value" id="coverState">-</div></div><div class="tile"><div class="tiny" data-i18n="light">Licht</div><div class="value" id="lightState">-</div></div><div class="tile"><div class="tiny" data-i18n="heater">Heizung</div><div class="value" id="heaterState">-</div></div><div class="tile"><div class="tiny">BME280</div><div class="value" id="bmeState">-</div></div></div></section>
<details id="networkDetails" class="wide"><summary data-i18n="network">Netzwerk</summary><form class="stack" method="POST" action="/setup/save"><label><span data-i18n="foundNetworks">Gefundene WLANs</span><select id="networks" onchange="document.getElementById('ssid').value=this.value"><option value="">)rawliteral";
    html += networksPreloaded ? "WLAN auswählen" : "SSID manuell eingeben";
    html += R"rawliteral(</option>)rawliteral";
    html += networkOptions;
    html += R"rawliteral(</select></label><div class="tiny" data-i18n="reloadToScan">Bitte Seite neu laden, um erneut nach WLANs zu scannen.</div><label>SSID<input id="ssid" name="ssid" autocomplete="username" value=")rawliteral";
    html += savedSsid;
    html += R"rawliteral("></label><label><span data-i18n="password">Passwort</span><input name="password" type="password" autocomplete="current-password"></label><div class="row"><button class="primary" type="submit" data-i18n="save">Speichern</button><button class="warn" type="submit" formaction="/setup/reset" data-i18n="resetWifi">WLAN zurücksetzen</button></div><div class="tiny" data-i18n="wifiSaveHint">Beim Speichern bleibt der Hotspot aktiv, bis die WLAN-Verbindung erfolgreich getestet wurde.</div></form></details>
<details id="logDetails" class="wide control-section"><summary data-i18n="monitorLog">Fehler / Monitor-Log</summary><div class="row" style="margin:12px 0"><button type="button" onclick="loadLogs()" data-i18n="refresh">Aktualisieren</button><button type="button" onclick="clearLogs()" data-i18n="clear">Leeren</button></div><pre id="monitorLog" class="logbox" data-i18n="logLoading">Log wird geladen...</pre></details>
<section class="control-section"><h2 data-i18n="cover">Deckel</h2><div class="row"><button class="primary" onclick="put('/api/v1/covercalibrator/0/opencover')" data-i18n="open">Öffnen</button><button class="primary" onclick="put('/api/v1/covercalibrator/0/closecover')" data-i18n="close">Schließen</button></div><div class="kv" style="margin-top:14px"><span>Servo Pin</span><strong>IO4</strong><span data-i18n="status">Status</span><strong id="coverState2">-</strong><span data-i18n="position">Position</span><strong id="mappedPos">-</strong></div></section>
<section class="control-section"><h2 data-i18n="lightPanel">Lichtpanel</h2><div class="row"><button class="primary" onclick="panelOn()" data-i18n="on">Ein</button><button onclick="put('/api/v1/covercalibrator/0/calibratoroff')" data-i18n="off">Aus</button></div><label style="margin-top:12px"><span data-i18n="brightness">Helligkeit</span> <span id="brightnessText">0</span><input id="brightness" type="range" min="0" max="255" value="0" oninput="setText('brightnessText',this.value)" onchange="setBrightness(this.value)"></label><div class="tiny" data-i18n="panelPwmHint">Panel PWM liegt auf IO13.</div></section>
<section class="control-section"><h2 data-i18n="environment">Umgebung</h2><div class="grid"><div class="tile"><div class="tiny" data-i18n="temperature">Temperatur</div><div class="value" id="temp">-</div></div><div class="tile"><div class="tiny" data-i18n="humidity">Luftfeuchte</div><div class="value" id="hum">-</div></div><div class="tile"><div class="tiny" data-i18n="pressure">Luftdruck</div><div class="value" id="pres">-</div></div><div class="tile"><div class="tiny" data-i18n="dewPoint">Taupunkt</div><div class="value" id="dew">-</div></div></div></section>
<section class="control-section"><h2 data-i18n="heater">Heizung</h2><div class="row"><button class="primary" onclick="heaterAuto(true)" data-i18n="dewAuto">Taupunkt Auto</button><button onclick="heaterAuto(false)" data-i18n="autoOff">Auto Aus</button><button onclick="heaterManual(true)" data-i18n="manualOn">Manuell Ein</button><button onclick="heaterManual(false)" data-i18n="manualOff">Manuell Aus</button></div><div class="kv" style="margin-top:14px"><span data-i18n="output">Ausgang</span><strong id="heaterOutput">-</strong><span data-i18n="mode">Modus</span><strong id="heaterMode">-</strong><span data-i18n="autoActive">Auto aktiv</span><strong id="heaterAuto">-</strong></div></section>
<details class="wide control-section"><summary data-i18n="positionCalibration">Positionskalibrierung</summary><div class="grid" style="margin:12px 0"><div class="tile"><div class="tiny" data-i18n="servoPulse">Servo Puls</div><div class="value" id="servoPulseTile">-</div></div><div class="tile"><div class="tiny" data-i18n="adcRaw">ADC roh</div><div class="value" id="rawPos">-</div></div><div class="tile"><div class="tiny" data-i18n="adcFiltered">ADC gefiltert</div><div class="value" id="filteredPos">-</div></div><div class="tile"><div class="tiny" data-i18n="position">Position</div><div class="value" id="mappedPos2">-</div></div></div><label><span data-i18n="manualServo">Servo manuell fahren</span> <span id="servoPulseText">-</span><input id="servoPulse" type="range" min="500" max="2500" step="10" value="1500" oninput="servoPreview(this.value)" onchange="servoSet(this.value)"></label><label class="inline" style="margin-top:10px"><input id="servoSmooth" type="checkbox" onchange="servoSmoothSet(this.checked)"><span data-i18n="servoSmooth">Servo sanft anfahren/abfahren</span></label><label style="margin-top:10px"><span data-i18n="maxSpeed">Maximalgeschwindigkeit</span> <span id="servoSpeedText">-</span><input id="servoSpeed" type="range" min="100" max="3000" step="50" value="1000" oninput="servoSpeedPreview(this.value)" onchange="servoSpeedSet(this.value)"></label><div class="row" style="margin:10px 0 14px"><button onclick="servoStep(-100)">-100 us</button><button onclick="servoStep(-10)">-10 us</button><button onclick="servoStep(10)">+10 us</button><button onclick="servoStep(100)">+100 us</button><button onclick="servoSet(servoOpenUs)" data-i18n="openPulse">Offen-Puls</button><button onclick="servoSet(servoCloseUs)" data-i18n="closedPulse">Geschlossen-Puls</button></div><div class="row"><button onclick="put('/api/v1/covercalibrator/0/opencover')" data-i18n="openCover">Deckel öffnen</button><button class="primary" onclick="put('/api/v1/flatpanel/calibrate/open')" data-i18n="saveAsOpen">Aktuell als Offen speichern</button><button onclick="put('/api/v1/covercalibrator/0/closecover')" data-i18n="closeCover">Deckel schließen</button><button class="primary" onclick="put('/api/v1/flatpanel/calibrate/closed')" data-i18n="saveAsClosed">Aktuell als Geschlossen speichern</button><button class="warn" onclick="put('/api/v1/flatpanel/calibrate/reset')" data-i18n="resetCalibration">Kalibrierung zurücksetzen</button></div><div class="grid" style="margin-top:12px"><div class="tile"><div class="tiny" data-i18n="openRaw">Offen RAW</div><div class="value" id="openRaw">-</div></div><div class="tile"><div class="tiny" data-i18n="closedRaw">Geschlossen RAW</div><div class="value" id="closedRaw">-</div></div><div class="tile"><div class="tiny" data-i18n="openServo">Offen Servo</div><div class="value" id="openServo">-</div></div><div class="tile"><div class="tiny" data-i18n="closedServo">Geschlossen Servo</div><div class="value" id="closedServo">-</div></div></div><div class="tiny" style="margin-top:10px" data-i18n="calibrationHint">Ablauf: mit dem manuellen Servo-Regler exakt auf Offen fahren, Offen speichern, dann exakt auf Geschlossen fahren und Geschlossen speichern.</div></details>
<section class="wide control-section"><h2 data-i18n="system">System</h2><div class="row"><a class="pill" href="/management/v1/configureddevices" data-i18n="alpacaDevices">Alpaca Geräte</a><a class="pill" href="/api/v1/flatpanel/environment">JSON Status</a><a class="pill" href="/monitor/log">Monitor Log</a></div><div class="tiny" style="margin-top:10px" data-i18n="otaHint">OTA-Upload über PlatformIO: Environment esp32s2_ota, Hostname flipflatpanel.local oder die aktuelle IP.</div></section>
</main><script src="/setup/app.js"></script></body></html>)rawliteral";
    return html;
}

static const char SETUP_APP_JS[] PROGMEM = R"rawliteral(
var I={de:{pageTitle:'FlipFlatpanel Setup',setupHotspot:'Setup-Hotspot',apPassword:'AP-Passwort',networkShort:'Netzwerk',heater:'Heizung',language:'Sprache',status:'Status',cover:'Deckel',light:'Licht',network:'Netzwerk',foundNetworks:'Gefundene WLANs',selectNetwork:'WLAN auswählen',manualOrScan:'SSID manuell eingeben',reloadToScan:'Bitte Seite neu laden, um erneut nach WLANs zu scannen.',password:'Passwort',save:'Speichern',resetWifi:'WLAN zurücksetzen',wifiSaveHint:'Beim Speichern bleibt der Hotspot aktiv, bis die WLAN-Verbindung erfolgreich getestet wurde.',monitorLog:'Fehler / Monitor-Log',refresh:'Aktualisieren',clear:'Leeren',logLoading:'Log wird geladen...',logEmpty:'Noch keine Meldungen.',logUnavailable:'Log nicht erreichbar',open:'Öffnen',close:'Schließen',position:'Position',lightPanel:'Lichtpanel',on:'Ein',off:'Aus',brightness:'Helligkeit',panelPwmHint:'Panel PWM liegt auf IO13.',environment:'Umgebung',temperature:'Temperatur',humidity:'Luftfeuchte',pressure:'Luftdruck',dewPoint:'Taupunkt',dewAuto:'Taupunkt Auto',autoOff:'Auto Aus',manualOn:'Manuell Ein',manualOff:'Manuell Aus',output:'Ausgang',mode:'Modus',autoActive:'Auto aktiv',positionCalibration:'Positionskalibrierung',servoPulse:'Servo Puls',adcRaw:'ADC roh',adcFiltered:'ADC gefiltert',manualServo:'Servo manuell fahren',servoSmooth:'Servo sanft anfahren/abfahren',maxSpeed:'Maximalgeschwindigkeit',openPulse:'Offen-Puls',closedPulse:'Geschlossen-Puls',openCover:'Deckel öffnen',closeCover:'Deckel schließen',saveAsOpen:'Aktuell als Offen speichern',saveAsClosed:'Aktuell als Geschlossen speichern',resetCalibration:'Kalibrierung zurücksetzen',openRaw:'Offen RAW',closedRaw:'Geschlossen RAW',openServo:'Offen Servo',closedServo:'Geschlossen Servo',calibrationHint:'Ablauf: mit dem manuellen Servo-Regler exakt auf Offen fahren, Offen speichern, dann exakt auf Geschlossen fahren und Geschlossen speichern.',system:'System',alpacaDevices:'Alpaca Geräte',otaHint:'OTA-Upload über PlatformIO: Environment esp32s2_ota, Hostname flipflatpanel.local oder die aktuelle IP.',httpOk:'HTTP OK',wlan:'WLAN',setupHotspotState:'Setup-Hotspot',noConnection:'Keine Verbindung',missing:'Fehlt',ready:'Bereit',notReady:'Nicht bereit',io4Active:'IO4 aktiv',io13Active:'IO13 aktiv',legendOk:'OK',legendPending:'Hinweis',legendBad:'Fehler',manualOnState:'Manuell ein',manualOffState:'Manuell aus',dewAutomation:'Taupunkt-Automatik',automationOff:'Automatik aus',unreachable:'Nicht erreichbar',statusUnavailable:'Status nicht abrufbar',coverOpen:'Offen',coverClosed:'Geschlossen',coverMoving:'Bewegt',coverUnknown:'Unbekannt',coverError:'Fehler',isOn:'AN',isOff:'AUS',manual:'Manuell',dewPointMode:'Taupunkt'},en:{pageTitle:'FlipFlatpanel Setup',setupHotspot:'Setup hotspot',apPassword:'AP password',networkShort:'Network',heater:'Heater',language:'Language',status:'Status',cover:'Cover',light:'Light',network:'Network',foundNetworks:'Found Wi-Fi networks',selectNetwork:'Select Wi-Fi',manualOrScan:'Enter SSID manually',reloadToScan:'Reload this page to scan for Wi-Fi networks again.',password:'Password',save:'Save',resetWifi:'Reset Wi-Fi',wifiSaveHint:'When saving, the setup hotspot remains active until the Wi-Fi connection has been tested successfully.',monitorLog:'Errors / Monitor log',refresh:'Refresh',clear:'Clear',logLoading:'Loading log...',logEmpty:'No messages yet.',logUnavailable:'Log unavailable',open:'Open',close:'Close',position:'Position',lightPanel:'Light panel',on:'On',off:'Off',brightness:'Brightness',panelPwmHint:'Panel PWM is on IO13.',environment:'Environment',temperature:'Temperature',humidity:'Humidity',pressure:'Pressure',dewPoint:'Dew point',dewAuto:'Dew point auto',autoOff:'Auto off',manualOn:'Manual on',manualOff:'Manual off',output:'Output',mode:'Mode',autoActive:'Auto active',positionCalibration:'Position calibration',servoPulse:'Servo pulse',adcRaw:'ADC raw',adcFiltered:'ADC filtered',manualServo:'Manual servo control',servoSmooth:'Smooth servo start/stop',maxSpeed:'Maximum speed',openPulse:'Open pulse',closedPulse:'Closed pulse',openCover:'Open cover',closeCover:'Close cover',saveAsOpen:'Save current as open',saveAsClosed:'Save current as closed',resetCalibration:'Reset calibration',openRaw:'Open RAW',closedRaw:'Closed RAW',openServo:'Open servo',closedServo:'Closed servo',calibrationHint:'Workflow: use the manual servo slider to move exactly to the open position, save open, then move exactly to the closed position and save closed.',system:'System',alpacaDevices:'Alpaca devices',otaHint:'OTA upload via PlatformIO: environment esp32s2_ota, hostname flipflatpanel.local or the current IP.',httpOk:'HTTP OK',wlan:'Wi-Fi',setupHotspotState:'Setup hotspot',noConnection:'No connection',missing:'Missing',ready:'Ready',notReady:'Not ready',io4Active:'IO4 active',io13Active:'IO13 active',legendOk:'OK',legendPending:'Notice',legendBad:'Error',manualOnState:'Manual on',manualOffState:'Manual off',dewAutomation:'Dew point automation',automationOff:'Automation off',unreachable:'Unreachable',statusUnavailable:'Status unavailable',coverOpen:'Open',coverClosed:'Closed',coverMoving:'Moving',coverUnknown:'Unknown',coverError:'Error',isOn:'ON',isOff:'OFF',manual:'Manual',dewPointMode:'Dew point'}};
var lang='de',servoPulseUs=1500,servoMinUs=500,servoMaxUs=2500,servoOpenUs=2500,servoCloseUs=500,servoSpeed=1000,servoSpeedMin=100,servoSpeedMax=3000;
try{lang=localStorage.getItem('flipflatpanel_lang')||'de'}catch(e){}
function el(id){return document.getElementById(id)}function tr(k){return (I[lang]&&I[lang][k])||I.de[k]||k}function setText(id,v){var e=el(id);if(e)e.textContent=v}function val(v,f){return v===undefined||v===null?f:v}
function xhr(m,u,b,cb){var x=new XMLHttpRequest();x.open(m,u,true);x.setRequestHeader('Content-Type','application/x-www-form-urlencoded');x.onreadystatechange=function(){if(x.readyState===4&&cb)cb(x)};x.send(b||'')}
function applyLanguage(){document.documentElement.lang=lang;var s=el('languageSelect');if(s)s.value=lang;var nodes=document.querySelectorAll('[data-i18n]');for(var i=0;i<nodes.length;i++)nodes[i].textContent=tr(nodes[i].getAttribute('data-i18n'));var n=el('networks');if(n&&n.options.length&&n.options[0].value==='')n.options[0].textContent=(document.body.getAttribute('data-networks-preloaded')==='1')?tr('selectNetwork'):tr('manualOrScan')}
function setLanguage(l){lang=(l==='en')?'en':'de';try{localStorage.setItem('flipflatpanel_lang',lang)}catch(e){}applyLanguage();refresh()}
function put(u,b){xhr('PUT',u,b,function(){setTimeout(refresh,150)})}function panelOn(){el('brightness').value=255;setText('brightnessText','255');put('/api/v1/covercalibrator/0/calibratoron','Brightness=255')}function setBrightness(v){put('/api/v1/covercalibrator/0/brightness','Brightness='+encodeURIComponent(v))}
function heaterAuto(e){put('/api/v1/flatpanel/heaterenabled','Enabled='+(e?'true':'false'))}function heaterManual(o){put('/api/v1/flatpanel/heatermanual','On='+(o?'true':'false'))}
function servoPreview(v){setText('servoPulseText',v+' us');setText('servoPulseTile',v+' us')}function servoSet(v){v=Math.max(servoMinUs,Math.min(servoMaxUs,Number(v)||servoPulseUs));servoPulseUs=v;el('servoPulse').value=v;servoPreview(v);put('/api/v1/flatpanel/servo','PulseUs='+encodeURIComponent(v))}function servoStep(d){servoSet(servoPulseUs+d)}function servoSmoothSet(e){put('/api/v1/flatpanel/servosmooth','Enabled='+(e?'true':'false'))}function servoSpeedPreview(v){setText('servoSpeedText',v+' us/s')}function servoSpeedSet(v){servoSpeed=Math.max(servoSpeedMin,Math.min(servoSpeedMax,Number(v)||servoSpeed));el('servoSpeed').value=servoSpeed;servoSpeedPreview(servoSpeed);put('/api/v1/flatpanel/servospeed','MaxSpeedUsPerSec='+encodeURIComponent(servoSpeed))}
function fmt(v,d,u){return Number.isFinite(v)?Number(v).toFixed(d)+u:'-'}function coverLabel(v){v=String(v||'').toLowerCase();if(v==='offen'||v==='open')return tr('coverOpen');if(v==='geschlossen'||v==='closed')return tr('coverClosed');if(v==='moving'||v==='bewegt')return tr('coverMoving');if(v==='error'||v==='fehler')return tr('coverError');return tr('coverUnknown')}function setSystem(id,state,label){var dot=el(id+'Dot');if(dot)dot.className='dot '+state;setText(id,label)}
function updateSystemStatus(d){setSystem('sysAlpaca','ok',tr('httpOk'));if(d.networkConnected){var r=Number.isFinite(d.networkRssi)?' · '+d.networkRssi+' dBm':'';setSystem('sysNetwork','ok',(d.networkSsid||tr('wlan'))+' · '+(d.networkIp||'-')+r)}else if(d.accessPointActive){setSystem('sysNetwork','pending',tr('setupHotspotState')+' · '+(d.accessPointIp||'192.168.4.1'))}else setSystem('sysNetwork','bad',tr('noConnection'));setSystem('sysBme',d.bmeAvailable?'ok':'bad',d.bmeAvailable?'OK':tr('missing'));setSystem('sysOta',d.otaReady?'ok':'bad',d.otaReady?tr('ready'):tr('notReady'));if(d.heaterManualMode)setSystem('sysHeater',d.heaterOn?'ok':'pending',d.heaterOn?tr('manualOnState'):tr('manualOffState'));else setSystem('sysHeater',d.heaterEnabled?'ok':'pending',d.heaterEnabled?tr('dewAutomation'):tr('automationOff'))}
function refresh(){xhr('GET','/api/v1/flatpanel/environment?ts='+(new Date().getTime()),'',function(x){try{var d=JSON.parse(x.responseText);updateSystemStatus(d);var c=coverLabel(d.coverStateText);setText('coverState',c);setText('coverState2',c);setText('lightState',d.brightness>0?tr('isOn'):tr('isOff'));setText('brightnessText',val(d.brightness,0));if(document.activeElement!==el('brightness'))el('brightness').value=val(d.brightness,0);servoMinUs=val(d.servoMinUs,servoMinUs);servoMaxUs=val(d.servoMaxUs,servoMaxUs);servoOpenUs=val(d.servoOpenUs,servoOpenUs);servoCloseUs=val(d.servoCloseUs,servoCloseUs);servoPulseUs=val(d.servoPulseUs,servoPulseUs);el('servoPulse').min=servoMinUs;el('servoPulse').max=servoMaxUs;if(document.activeElement!==el('servoPulse'))el('servoPulse').value=servoPulseUs;servoPreview(el('servoPulse').value);if(el('servoSmooth'))el('servoSmooth').checked=!!d.servoSmoothEnabled;servoSpeedMin=val(d.servoSpeedMinUsPerSec,servoSpeedMin);servoSpeedMax=val(d.servoSpeedMaxUsPerSec,servoSpeedMax);servoSpeed=val(d.servoMaxSpeedUsPerSec,servoSpeed);el('servoSpeed').min=servoSpeedMin;el('servoSpeed').max=servoSpeedMax;if(document.activeElement!==el('servoSpeed'))el('servoSpeed').value=servoSpeed;servoSpeedPreview(el('servoSpeed').value);setText('heaterState',d.heaterOn?tr('isOn'):tr('isOff'));setText('heaterOutput',d.heaterOn?tr('isOn'):tr('isOff'));setText('heaterMode',d.heaterManualMode?tr('manual'):tr('dewPointMode'));setText('heaterAuto',d.heaterEnabled?tr('isOn'):tr('isOff'));setText('bmeState',d.bmeAvailable?'OK':tr('missing'));setText('temp',fmt(d.temperatureC,1,' °C'));setText('hum',fmt(d.humidityPct,1,' %'));setText('pres',fmt(d.pressureHpa,1,' hPa'));setText('dew',fmt(d.dewPointC,1,' °C'));setText('mappedPos',Number.isFinite(d.mappedPosition)?d.mappedPosition+' %':'-');setText('mappedPos2',Number.isFinite(d.mappedPosition)?d.mappedPosition+' %':'-');setText('rawPos',val(d.rawPosition,'-'));setText('filteredPos',val(d.filteredPosition,'-'));setText('openRaw',val(d.calibrationOpenRaw,'-'));setText('closedRaw',val(d.calibrationClosedRaw,'-'));setText('openServo',Number.isFinite(d.calibrationOpenServoUs)?d.calibrationOpenServoUs+' us':'-');setText('closedServo',Number.isFinite(d.calibrationClosedServoUs)?d.calibrationClosedServoUs+' us':'-')}catch(e){setSystem('sysAlpaca','bad',tr('unreachable'))}})}
function loadLogs(){xhr('GET','/monitor/log?ts='+(new Date().getTime()),'',function(x){setText('monitorLog',x.status===200?(x.responseText||tr('logEmpty')):tr('logUnavailable'))})}function clearLogs(){xhr('POST','/monitor/log/clear','',function(){loadLogs()})}
var networkScanLoaded=false;function loadNetworks(){var s=el('networks');if(!s)return;if(networkScanLoaded)return;networkScanLoaded=true;s.innerHTML='<option value="">Suche läuft...</option>';xhr('GET','/setup/networks?refresh=1&ts='+(new Date().getTime()),'',function(x){try{var d=JSON.parse(x.responseText),list=d.networks||[];s.innerHTML='<option value="">'+tr('selectNetwork')+'</option>';for(var i=0;i<list.length;i++){var o=document.createElement('option');o.value=list[i].ssid;o.textContent=list[i].ssid+' ('+list[i].rssi+' dBm)';s.appendChild(o)}if(!list.length&&(d.scanning||d.requested)){networkScanLoaded=false;setTimeout(loadNetworks,1200)}else if(!list.length){s.innerHTML='<option value="">'+tr('manualOrScan')+'</option>'}}catch(e){s.innerHTML='<option value="">'+tr('manualOrScan')+'</option>'}})}
function updateNetworkMode(){var d=el('networkDetails');if(d&&d.open){document.body.className='network-active';loadNetworks()}else document.body.className=''}
var nd=el('networkDetails');if(nd)nd.addEventListener('toggle',updateNetworkMode);var ld=el('logDetails');if(ld)ld.addEventListener('toggle',function(){if(ld.open)loadLogs()});applyLanguage();updateNetworkMode();setTimeout(refresh,400);
)rawliteral";

static void sendSetupPage(AsyncWebServerRequest* request) {
    String savedSsid = preferences.isKey("ssid") ? htmlEscape(preferences.getString("ssid", "")) : "";
    const bool hasCachedNetworks = !networkScanOptions.isEmpty();
    String html = buildSplitSetupPage(savedSsid, networkScanOptions, hasCachedNetworks);
    Monitor::print("[HTTP] Setup-Seite bytes=");
    Monitor::println(html.length());
    AsyncWebServerResponse* response = request->beginResponse(200, "text/html; charset=utf-8", html);
    response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    request->send(response);
}

static void startAccessPoint() {
    Monitor::println("[WiFi] Starte Access Point...");

    dnsServer.stop();

    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    delay(200);
    WiFi.mode(WIFI_OFF);
    delay(500);

    currentApSsid = AP_SSID;

    bool apStarted = false;
    for (int attempt = 0; attempt < 5 && !apStarted; ++attempt) {
        Monitor::print("[WiFi] AP Versuch ");
        Monitor::println(attempt + 1);
        WiFi.mode(WIFI_AP);
        setWiFiTxPower(WIFI_AP_SCAN_TX_POWER);
        delay(300);
        const bool apConfigOk = WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        apStarted = apConfigOk && WiFi.softAP(currentApSsid.c_str(), AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CLIENTS);
        delay(1000);
    }

    if (apStarted) {
        Monitor::print("[WiFi] AP gestartet SSID: ");
        Monitor::println(currentApSsid);
        Monitor::print("[WiFi] AP Passwort: ");
        Monitor::println(AP_PASSWORD);
        Monitor::print("[WiFi] Setup URL: http://");
        Monitor::print(WiFi.softAPIP());
        Monitor::println("/setup");
    } else {
        Monitor::println("[WiFi] AP Start fehlgeschlagen!");
    }

    if (apStarted) {
        const bool dnsStarted = dnsServer.start(53, "*", apIP);
        Monitor::print("[WiFi] Captive DNS: ");
        Monitor::println(dnsStarted ? "gestartet" : "fehlgeschlagen");
    }
}


static void maintainStationConnection() {
    if (configuredSsid.isEmpty() || isAccessPointActive()) return;

    const unsigned long now = millis();
    if (now - lastWiFiCheckMs < WIFI_CHECK_INTERVAL_MS) return;
    lastWiFiCheckMs = now;

    if (WiFi.status() == WL_CONNECTED) {
        if (!stationWasConnected) {
            Monitor::print("[WiFi] Verbindung wiederhergestellt, IP: ");
            Monitor::println(WiFi.localIP());
        }
        stationWasConnected = true;
        wifiDisconnectedSinceMs = 0;
        return;
    }

    if (wifiDisconnectedSinceMs == 0) {
        wifiDisconnectedSinceMs = now;
        Monitor::print("[WiFi] Verbindung verloren, Status=");
        Monitor::println(static_cast<int>(WiFi.status()));
    }
    stationWasConnected = false;

    if (now - wifiDisconnectedSinceMs >= WIFI_AP_FALLBACK_MS) {
        Monitor::println("[WiFi] Reconnect erfolglos -> starte Setup AP");
        WiFi.disconnect(true, true);
        delay(200);
        WiFi.setHostname(DEVICE_NAME);
        startAccessPoint();
        return;
    }

    if (now - lastReconnectAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
        lastReconnectAttemptMs = now;
        Monitor::print("[WiFi] Reconnect zu SSID: ");
        Monitor::println(configuredSsid);
        WiFi.disconnect(false, false);
        delay(100);
        WiFi.mode(WIFI_STA);
        setWiFiTxPower(WIFI_STA_TX_POWER);
        WiFi.setHostname(DEVICE_NAME);
        WiFi.begin(configuredSsid.c_str(), configuredPassword.c_str());
    }
}

void initWiFi() {
    Monitor::println("\n[WiFi] Init...");

    preferences.begin("wifi_config", false);

    configuredSsid = preferences.isKey("ssid") ? preferences.getString("ssid", "") : "";
    configuredPassword = preferences.isKey("password") ? preferences.getString("password", "") : "";
    const String savedSsid = configuredSsid;
    const String savedPassword = configuredPassword;

    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    if (savedSsid.isEmpty()) {
        Monitor::println("[WiFi] Keine gespeicherte SSID -> starte Setup AP");
        WiFi.setHostname(DEVICE_NAME);
        startAccessPoint();
        return;
    }

    WiFi.mode(WIFI_STA);
    setWiFiTxPower(WIFI_STA_TX_POWER);
    WiFi.setHostname(DEVICE_NAME);

    Monitor::print("[WiFi] Verbinde mit gespeicherter SSID: ");
    Monitor::println(savedSsid);

    WiFi.begin(savedSsid.c_str(), savedPassword.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
        Monitor::print(".");
        delay(500);
    }
    Monitor::println();

    if (WiFi.status() == WL_CONNECTED) {
        stationWasConnected = true;
        wifiDisconnectedSinceMs = 0;
        Monitor::println("[WiFi] Verbunden!");
        Monitor::print("[WiFi] IP: ");
        Monitor::println(WiFi.localIP());
    } else {
        Monitor::println("[WiFi] Verbindung fehlgeschlagen -> starte Setup AP");
        WiFi.setHostname(DEVICE_NAME);
        startAccessPoint();
    }
}

void processDNS() {
    maintainStationConnection();
    handleNetworkScan();
    if (isAccessPointActive()) {
        dnsServer.processNextRequest();
    }
}

String getIPAddress() {
    if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
    if (isAccessPointActive()) return WiFi.softAPIP().toString();
    return "0.0.0.0";
}

void setupWiFiEndpoints(AsyncWebServer& server) {
    server.on("/monitor/log", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(200, "text/plain; charset=utf-8", Monitor::getLog());
        response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        request->send(response);
    });

    server.on("/monitor/log/clear", HTTP_POST, [](AsyncWebServerRequest* request) {
        Monitor::clearLog();
        request->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/setup/app.js", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(200, "application/javascript; charset=utf-8", SETUP_APP_JS);
        response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        request->send(response);
    });

    server.on("/setup", HTTP_GET, sendSetupPage);
    server.on("/setup/v1/covercalibrator/0/setup", HTTP_GET, sendSetupPage);

    server.on("/generate_204", HTTP_GET, redirectToSetup);
    server.on("/gen_204", HTTP_GET, redirectToSetup);
    server.on("/hotspot-detect.html", HTTP_GET, redirectToSetup);
    server.on("/library/test/success.html", HTTP_GET, redirectToSetup);
    server.on("/success.txt", HTTP_GET, redirectToSetup);
    server.on("/ncsi.txt", HTTP_GET, redirectToSetup);
    server.on("/connecttest.txt", HTTP_GET, redirectToSetup);
    server.on("/redirect", HTTP_GET, redirectToSetup);
    server.on("/canonical.html", HTTP_GET, redirectToSetup);

    server.on("/setup/networks", HTTP_GET, [](AsyncWebServerRequest* request) {
        const bool force = request->hasParam("refresh");
        requestNetworkScan(force);
        pollNetworkScan();

        String response = "{\"scanning\":";
        response += (networkScanRunning || networkScanRequested) ? "true" : "false";
        response += ",\"error\":\"";
        response += jsonEscape(networkScanError);
        response += "\"";
        response += ",\"requested\":";
        response += networkScanRequested ? "true" : "false";
        response += ",\"requestAgeMs\":";
        response += String(millis() - networkScanRequestedMs);
        response += ",\"cacheAgeMs\":";
        response += String(millis() - lastNetworkScanMs);
        response += ",\"networks\":";
        response += networkScanJson;
        response += "}";
        request->send(200, "application/json", response);
    });

    server.on("/setup/save", HTTP_POST, [](AsyncWebServerRequest* request) {
        String ssid, password;
        if (request->hasParam("ssid", true)) ssid = request->getParam("ssid", true)->value();
        if (request->hasParam("password", true)) password = request->getParam("password", true)->value();

        ssid.trim();
        if (ssid.isEmpty()) {
            request->send(400, "text/html; charset=utf-8", "<html><body><h2>Keine SSID angegeben</h2><p>Der Setup-Hotspot bleibt aktiv.</p><p><a href='/setup?portal=1'>Zurueck zum Setup</a></p></body></html>");
            return;
        }

        const bool connected = connectStation(ssid, password, 20000);
        if (!connected) {
            request->send(200, "text/html; charset=utf-8", "<html><body><h2>WLAN-Verbindung fehlgeschlagen</h2><p>Die Daten wurden nicht gespeichert. Der Setup-Hotspot bleibt aktiv.</p><p><a href='/setup?portal=1'>Zurueck zum Setup</a></p></body></html>");
            return;
        }

        preferences.putString("ssid", ssid);
        preferences.putString("password", password);
        configuredSsid = ssid;
        configuredPassword = password;
        stationWasConnected = true;
        wifiDisconnectedSinceMs = 0;
        String html = "<html><body><h2>WLAN verbunden</h2><p>Gespeichert. IP: ";
        html += WiFi.localIP().toString();
        html += "</p><p>Der Setup-Hotspot wird jetzt beendet.</p><p>Setup danach: <a href='http://";
        html += WiFi.localIP().toString();
        html += "/setup'>http://";
        html += WiFi.localIP().toString();
        html += "/setup</a></p></body></html>";
        request->send(200, "text/html; charset=utf-8", html);
        stopAccessPointAfterResponse();
    });

    server.on("/setup/reset", HTTP_POST, [](AsyncWebServerRequest* request) {
        preferences.remove("ssid");
        preferences.remove("password");
        configuredSsid = "";
        configuredPassword = "";
        stationWasConnected = false;
        wifiDisconnectedSinceMs = 0;
        if (!isAccessPointActive()) startAccessPoint();
        request->send(200, "text/html; charset=utf-8", "<html><body><h2>WLAN geloescht</h2><p>Der Setup-Hotspot bleibt aktiv.</p><p><a href='/setup?portal=1'>Zurueck zum Setup</a></p></body></html>");
    });

    server.onNotFound([](AsyncWebServerRequest* request) {
        if (isAccessPointActive()) {
            redirectToSetup(request);
            return;
        }
        request->send(404, "text/plain; charset=utf-8", "Not found");
    });
}
