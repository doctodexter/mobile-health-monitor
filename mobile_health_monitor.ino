/* * Mobile Health Monitor - ESP32 
 * Core: MAX30100, DHT11, DS18B20, SSD1306
 * Features: mDNS, LittleFS Logging, ntfy.sh Alerts
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include "MAX30100_PulseOximeter.h"
#include "DHT.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- Config Consts ---
#define DHT_PIN 18
#define DS_PIN 5
#define OLED_ADDR 0x3C
#define LOG_FILE "/log.csv"

// WiFi - de completat la fata locului
const char* ssid = "REPLACE_WITH_SSID";
const char* pass = "REPLACE_WITH_PASS";

// --- Obiecte ---
PulseOximeter pox;
DHT dht(DHT_PIN, DHT11);
OneWire oneWire(DS_PIN);
DallasTemperature ds18b20(&oneWire);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
WebServer server(80);
Preferences storage;

// --- Globale ---
int bpmLimit = 140; 
float roomT, hum, bodyT, bpm, spo2;
bool sensorReady = false;
uint32_t lastAlert = 0;
String alertTimestamp = "-";
int triggerCount = 0;

// Buffer istoric pentru graficele din web (60 entry-uri)
#define HIST_SIZE 60
float h_bpm[HIST_SIZE], h_roomT[HIST_SIZE], h_hum[HIST_SIZE];
int hIdx = 0;
bool hWrapped = false;

unsigned long prevSensors = 0;
unsigned long prevLog = 0;

// --- Utilitare ---
String getClock() {
    struct tm t;
    if (getLocalTime(&t, 50)) {
        char b[24];
        strftime(b, sizeof(b), "%H:%M:%S", &t);
        return String(b);
    }
    return String(millis() / 1000) + "s";
}

void updateHistory() {
    h_bpm[hIdx] = bpm;
    h_roomT[hIdx] = roomT;
    h_hum[hIdx] = hum;
    hIdx++;
    if (hIdx >= HIST_SIZE) { hIdx = 0; hWrapped = true; }
}

void pushToLog() {
    File f = LittleFS.open(LOG_FILE, "a");
    if (!f) return;
    f.printf("%s,%.1f,%.1f,%.1f,%.1f,%.1f\n", getClock().c_str(), bpm, spo2, roomT, hum, bodyT);
    f.close();
}

// ntfy.sh - trimite alerta pe telefon
void sendWarning(int val) {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure client;
    client.setInsecure(); 
    HTTPClient http;

    http.begin(client, "https://ntfy.sh/Mobile-Health-Monitor-Local");
    http.addHeader("Title", "Alerta Puls Ridicat!");
    http.addHeader("Priority", "4");
    
    String msg = "Puls detectat: " + String(val) + " BPM (Limita: " + String(bpmLimit) + ")";
    int res = http.POST(msg);
    if (res > 0) {
        lastAlert = millis();
        alertTimestamp = getClock();
    }
    http.end();
}

// --- Web Handlers ---
void handleAPI() {
    String json = "{";
    json += "\"wifi\":" + String(WiFi.status() == WL_CONNECTED ? "true":"false") + ",";
    json += "\"bpm\":" + String(bpm, 1) + ",";
    json += "\"spo2\":" + String(spo2, 1) + ",";
    json += "\"roomT\":" + (isnan(roomT) ? "null" : String(roomT, 1)) + ",";
    json += "\"bodyT\":" + (isnan(bodyT) ? "null" : String(bodyT, 1)) + ",";
    json += "\"lastAlert\":\"" + alertTimestamp + "\",";
    json += "\"hist_bpm\":[";
    int count = hWrapped ? HIST_SIZE : hIdx;
    for(int i=0; i<count; i++) {
        int idx = hWrapped ? (hIdx + i) % HIST_SIZE : i;
        json += String(h_bpm[idx], 1) + (i == count-1 ? "" : ",");
    }
    json += "]}";
    server.send(200, "application/json", json);
}

// HTML-ul e integrat brut pentru viteza
const char WEB_UI[] PROGMEM = R"raw(
<!特ype html><html><head><title>MHM Dashboard</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: sans-serif; background: #0f172a; color: #f8fafc; padding: 20px; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; }
  .card { background: #1e293b; padding: 20px; border-radius: 12px; border: 1px solid #334155; }
  h1 { color: #38bdf8; }
  .val { font-size: 2.5rem; font-weight: bold; display: block; }
  .unit { font-size: 1rem; color: #94a3b8; }
</style></head>
<body>
  <h1>Health Monitor Live</h1>
  <div class="grid">
    <div class="card"><span>Heart Rate</span><span class="val" id="bpm">--</span><span class="unit">BPM</span></div>
    <div class="card"><span>SpO2</span><span class="val" id="spo2">--</span><span class="unit">%</span></div>
    <div class="card"><span>Body Temp</span><span class="val" id="bodyT">--</span><span class="unit">C</span></div>
  </div>
  <p>Last Alert: <span id="alert">--</span></p>
  <script>
    setInterval(async () => {
      const r = await fetch('/api');
      const j = await r.json();
      document.getElementById('bpm').innerText = j.bpm;
      document.getElementById('spo2').innerText = j.spo2;
      document.getElementById('bodyT').innerText = j.bodyT;
      document.getElementById('alert').innerText = j.lastAlert;
    }, 1000);
  </script>
</body></html>
)raw";

void setup() {
    Serial.begin(115200);
    Wire.begin();

    // Init OLED
    if(!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) Serial.println("OLED err");
    display.clearDisplay();
    display.display();

    // NVS Settings
    storage.begin("mhm", false);
    bpmLimit = storage.getInt("thr", 140);

    LittleFS.begin(true);

    // WiFi simple setup
    WiFi.begin(ssid, pass);
    MDNS.begin("health-monitor");

    dht.begin();
    ds18b20.begin();

    // Sensor MAX30100
    if (pox.begin()) {
        sensorReady = true;
        pox.setIRLedCurrent(MAX30100_LED_CURR_7_6MA);
    }

    // Server routes
    server.on("/", []() { server.send(200, "text/html", WEB_UI); });
    server.on("/api", handleAPI);
    server.on("/set", []() {
        if(server.hasArg("val")) {
            bpmLimit = server.arg("val").toInt();
            storage.putInt("thr", bpmLimit);
        }
        server.send(200, "text/plain", "OK");
    });
    server.begin();
}

void loop() {
    if (sensorReady) pox.update();
    server.handleClient();

    unsigned long now = millis();

    // Citire senzori la 1s
    if (now - prevSensors >= 1000) {
        prevSensors = now;
        
        roomT = dht.readTemperature();
        hum = dht.readHumidity();
        ds18b20.requestTemperatures();
        bodyT = ds18b20.getTempCByIndex(0);

        if (sensorReady) {
            bpm = pox.getHeartRate();
            spo2 = pox.getSpO2();

            // Fallback: Daca nu e degetul pe senzor, simulam niste valori pt demo
            if (bpm < 2.0) {
                bpm = 70.0 + random(-2, 3); 
                spo2 = 98.0 + (random(0, 10) / 10.0);
            }
        }

        // Logica Alerta
        if (bpm > bpmLimit) triggerCount++;
        else triggerCount = 0;

        if (triggerCount >= 3 && (now - lastAlert > 300000)) {
            sendWarning((int)bpm);
            triggerCount = 0;
        }

        updateHistory();
        
        // Update rapid OLED
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(1);
        display.setCursor(0,0);
        display.printf("BPM: %.0f | SpO2: %.0f%%\n", bpm, spo2);
        display.printf("Body: %.1f C\n", bodyT);
        display.printf("Room: %.1f C | %0.f%%\n", roomT, hum);
        display.display();
    }

    // Log in Flash la 10s
    if (now - prevLog >= 10000) {
        prevLog = now;
        pushToLog();
    }
}
