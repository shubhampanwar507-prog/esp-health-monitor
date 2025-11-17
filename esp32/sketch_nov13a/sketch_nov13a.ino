

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <math.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <time.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

MAX30105 particleSensor;

// ---------- Wi-Fi ----------
const char* ssid = "IITI";
const char* password = "";

// ---------- Server Upload ----------
const char* serverUrl = "http://10.203.4.215:5000/upload";  // <<<< set your server IP:port

// ---------- BP sensor on Serial2 ----------
#define BP_RX 18
#define BP_TX 19

// ---------- Sampling ----------
#define BUFFER_SIZE 200
const unsigned long SAMPLE_PERIOD_MS = 10; // 100 Hz sampling

uint32_t redBuffer[BUFFER_SIZE];
uint32_t irBuffer[BUFFER_SIZE];
int bufHead = 0;
int samplesFilled = 0;
uint64_t redSum = 0;
uint64_t irSum = 0;

float spo2 = 0.0;
float lastValidSpO2 = 95.0;
float lastRawSpO2 = 0.0;
int spo2Samples = 0;

// BPM logic
const byte RATE_SIZE = 10;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute = 0;
int beatAvg = 0;

// BP
String lastBP = "N/A";
unsigned long lastBPTime = 0;
const unsigned long bpTimeout = 60000;

// Web server
WebServer server(80);

// Timing
unsigned long lastSampleMillis = 0;
unsigned long lastSerialPrint = 0;

// Background HTTP uploader
struct LogPacket {
  uint32_t ir, red;
  float spo2_raw, spo2_filtered;
  int bpm, beatAvg;
  String bp;
};
QueueHandle_t logQueue;

// User
String currentUser = "UNKNOWN";

// ---------- Pulse feature extraction ----------
#define BEAT_WINDOW 150  // ~1.5s at 100Hz
int beatBuf[BEAT_WINDOW];
int beatIndex = 0;
bool collectingBeat = false;

// Features available globally for httpTask to send
volatile int sysPeak = 120;
volatile int diaPeak = 80;
volatile float pulseArea = 0.0;
volatile int lastGlucoseTime = 0; // ms
String lastGlucose = "--";

// For thread safety of globals, we only assign atomic-sized values; it's acceptable here.

// Forward declarations
void updateDisplayPeriodically();
void readBP();
String htmlPage();
void httpTask(void *pvParameters);
void appendLogRow(unsigned long t_ms, uint32_t ir, uint32_t red,
                  float spo2_raw, float spo2_filtered,
                  int bpmInstant, int bpmAvg, const String &bp);
bool checkForBeat(long sample);

// ---------- HTTP upload task ----------
void httpTask(void *pvParameters) {
  LogPacket packet;
  for (;;) {
    if (xQueueReceive(logQueue, &packet, portMAX_DELAY)) {
      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(serverUrl);
        http.addHeader("Content-Type", "application/json");

        // Read current feature values (from globals)
        int sys_peak = sysPeak;
        int dia_peak = diaPeak;
        int ppg_signal = packet.ir;
        int pulse_area_local = (int) pulseArea;

        // Build JSON string
        String json = "{";
        json += "\"user_id\":\"" + currentUser + "\",";
        json += "\"ir\":" + String(packet.ir) + ",";
        json += "\"red\":" + String(packet.red) + ",";
        json += "\"spo2_raw\":" + String(packet.spo2_raw, 2) + ",";
        json += "\"spo2_filtered\":" + String(packet.spo2_filtered, 2) + ",";
        json += "\"bpm\":" + String(packet.bpm) + ",";
        json += "\"beatAvg\":" + String(packet.beatAvg) + ",";
        json += "\"bp\":\"" + packet.bp + "\",";

        // New ML feature fields (PPG + waveform peaks + pulse area + BP cuff values will also be sent)
        json += "\"ppg_signal\":" + String(ppg_signal) + ",";
        json += "\"pulse_area\":" + String(pulse_area_local) + ",";
        json += "\"sys_peak\":" + String(sys_peak) + ",";
        json += "\"dia_peak\":" + String(dia_peak) + ",";
        // Also send BP cuff numeric if available (parse lastBP if possible)
        int cuffSys = 0, cuffDia = 0;
        String bpstr = packet.bp;
        int slashIndex = bpstr.indexOf('/');
        if (slashIndex > 0) {
          cuffSys = bpstr.substring(0, slashIndex).toInt();
          cuffDia = bpstr.substring(slashIndex + 1).toInt();
        }
        json += "\"cuff_sys\":" + String(cuffSys) + ",";
        json += "\"cuff_dia\":" + String(cuffDia) + ",";

        json += "\"glucose_dummy\":0";
        json += "}";

        int httpCode = http.POST(json);
        if (httpCode > 0) {
          String resp = http.getString();
          // Expecting JSON like {"status":"ok","glucose_range":"NORMAL"}
          // Quick parse to find glucose_range value
          int idx = resp.indexOf("glucose_range");
          if (idx >= 0) {
            int colon = resp.indexOf(':', idx);
            int quote1 = resp.indexOf('"', colon);
            int quote2 = resp.indexOf('"', quote1 + 1);
            if (quote1 >= 0 && quote2 > quote1) {
              String g = resp.substring(quote1 + 1, quote2);
              lastGlucose = g;
              lastGlucoseTime = millis();
            }
          }
        } else {
          Serial.print("HTTP POST failed, error: ");
          Serial.println(httpCode);
        }
        http.end();
      } else {
        Serial.println("WiFi disconnected; uploader skipping");
      }
    }
  }
}

// ---------- Log enqueue ----------
void appendLogRow(unsigned long t_ms, uint32_t ir, uint32_t red,
                  float spo2_raw, float spo2_filtered,
                  int bpmInstant, int bpmAvg, const String &bp) {
  LogPacket packet = {ir, red, spo2_raw, spo2_filtered, bpmInstant, bpmAvg, bp};
  xQueueSend(logQueue, &packet, 0);
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println("Starting...");

  Serial2.begin(9600, SERIAL_8N1, BP_RX, BP_TX);

  // Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected. Dashboard: http://");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("esp32")) {
      Serial.println("mDNS responder started: http://esp32.local");
      MDNS.addService("http", "tcp", 80);
    } else {
      Serial.println("mDNS responder failed.");
    }
  } else {
    Serial.println("\nWiFi connect failed or timed out; continuing without WiFi.");
  }

  // I2C
  Wire.begin(21, 22);

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation failed");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(10, 10);
    display.println("Initializing...");
    display.display();
    delay(200);
  }

  // Sensor
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30105 not found. Check wiring/power.");
    while (1) delay(1000);
  }

  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x1F);
  particleSensor.setPulseAmplitudeIR(0x1F);
  particleSensor.setPulseAmplitudeGreen(0);

  // Web routes
  server.on("/", []() { server.send(200, "text/html", htmlPage()); });

  server.on("/vitals", []() {
    String json = "{";
    json += "\"bpm\":" + String(beatAvg) + ",";
    json += "\"spo2\":" + String(spo2, 1) + ",";
    json += "\"bp\":\"" + ((millis() - lastBPTime < bpTimeout) ? lastBP : "Stale") + "\",";
    json += "\"glucose_range\":\"" + lastGlucose + "\"";
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/setUser", []() {
    if (server.hasArg("user")) {
      currentUser = server.arg("user");
      server.send(200, "text/plain", "Active user set to: " + currentUser);
      Serial.println("Active user: " + currentUser);
    } else {
      server.send(400, "text/plain", "Missing user parameter");
    }
  });

  server.on("/startBP", []() {
    Serial2.println("START");
    server.send(200, "text/plain", "BP sensor triggered");
  });

  server.begin();

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Ready. Place finger.");
  display.display();

  for (byte i = 0; i < RATE_SIZE; i++) rates[i] = 0;

  // Create background upload queue & task
  logQueue = xQueueCreate(10, sizeof(LogPacket));
  xTaskCreatePinnedToCore(httpTask, "HTTP Uploader", 8192, NULL, 1, NULL, 1);
}

// ---------- main loop ----------
void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - lastSampleMillis >= SAMPLE_PERIOD_MS) {
    lastSampleMillis = now;

    long irValue = particleSensor.getIR();
    long redValue = particleSensor.getRed();

    if (irValue <= 0 || redValue <= 0) {
      readBP();
      updateDisplayPeriodically();
      return;
    }

    // Fill circular buffer
    if (samplesFilled < BUFFER_SIZE) {
      redBuffer[bufHead] = redValue;
      irBuffer[bufHead] = irValue;
      redSum += redValue;
      irSum += irValue;
      samplesFilled++;
      bufHead = (bufHead + 1) % BUFFER_SIZE;
    } else {
      int tail = bufHead;
      redSum = redSum - redBuffer[tail] + redValue;
      irSum = irSum - irBuffer[tail] + irValue;
      redBuffer[tail] = redValue;
      irBuffer[tail] = irValue;
      bufHead = (bufHead + 1) % BUFFER_SIZE;
    }

    // compute DC/AC values
    float redDC = (float)redSum / samplesFilled;
    float irDC = (float)irSum / samplesFilled;

    double redSqSum = 0.0;
    double irSqSum = 0.0;
    for (int i = 0; i < samplesFilled; i++) {
      double rAC = ((double)redBuffer[i]) - redDC;
      double iAC = ((double)irBuffer[i]) - irDC;
      redSqSum += rAC * rAC;
      irSqSum += iAC * iAC;
    }
    float redRMS = sqrt((float)(redSqSum / samplesFilled));
    float irRMS = sqrt((float)(irSqSum / samplesFilled));

    if (irDC > 3000 && irRMS > 20.0) {
      float ratio = (redRMS / redDC) / (irRMS / irDC);
      if (isfinite(ratio) && ratio > 0) {
        ratio = constrain(ratio, 0.3f, 1.5f);
        float spo2Calc = (-18.0f * ratio * ratio) + (22.0f * ratio) + 93.0f;
        spo2Calc = constrain(spo2Calc, 70.0f, 100.0f);

        float signalQuality = irRMS / irDC;
        if (signalQuality < 0.004f || irDC < 5000)
          spo2Calc = lastValidSpO2;

        if (fabs(spo2Calc - lastValidSpO2) > 2.0f)
          spo2Calc = lastValidSpO2 * 0.8f + spo2Calc * 0.2f;

        const float alpha = (signalQuality > 0.015f) ? 0.15f : 0.05f;
        lastValidSpO2 = lastValidSpO2 * (1.0f - alpha) + spo2Calc * alpha;

        spo2 = lastValidSpO2;
        lastRawSpO2 = spo2Calc;
        spo2Samples++;
      }
    }

    // Beat detection
    if (checkForBeat(irValue)) {
      long delta = millis() - lastBeat;
      lastBeat = millis();

      // start collecting beat waveform to buffer
      beatIndex = 0;
      collectingBeat = true;

      if (delta > 300 && delta < 1500) {
        beatsPerMinute = 60000.0 / delta;
        if (beatsPerMinute > 30 && beatsPerMinute < 220) {
          rates[rateSpot++] = (byte)beatsPerMinute;
          rateSpot %= RATE_SIZE;
          int sum = 0;
          byte validRates = 0;
          for (byte i = 0; i < RATE_SIZE; i++) {
            if (rates[i] > 0) {
              sum += rates[i];
              validRates++;
            }
          }
          beatAvg = (validRates > 0) ? sum / validRates : (int)beatsPerMinute;
        }
      }
    }

    // fill beat waveform buffer
    if (collectingBeat && beatIndex < BEAT_WINDOW) {
      beatBuf[beatIndex++] = irValue;
      if (beatIndex >= BEAT_WINDOW) {
        collectingBeat = false;
      }
    }

    // compute systolic/diastolic/pulse area periodically
    static unsigned long lastFeatureTime = 0;
    if (millis() - lastFeatureTime > 2000 && beatIndex > 30) {
      lastFeatureTime = millis();

      // Extract features
      int maxVal = 0, maxIdx = 0;
      for (int i = 0; i < beatIndex; i++) {
        if (beatBuf[i] > maxVal) {
          maxVal = beatBuf[i];
          maxIdx = i;
        }
      }
      // second peak search (after systolic)
      int secondMax = 0, secondIdx = -1;
      for (int i = maxIdx + 8; i < beatIndex; i++) {
        if (beatBuf[i] > secondMax) {
          secondMax = beatBuf[i];
          secondIdx = i;
        }
      }
      // baseline from first sample (simple)
      int baseline = beatBuf[0];
      float area = 0;
      for (int i = 0; i < beatIndex; i++) area += (beatBuf[i] - baseline);

      // set global features
      sysPeak = maxVal;
      diaPeak = (secondMax > 0) ? secondMax : baseline;
      pulseArea = area;
      // debug
      // Serial.printf("Features: sys=%d dia=%d area=%.1f\n", sysPeak, diaPeak, pulseArea);
    }

    readBP();

    if (now - lastSerialPrint >= 500) {
      lastSerialPrint = now;
      Serial.print("RAW ir:"); Serial.print(irValue);
      Serial.print(" red:"); Serial.print(redValue);
      Serial.print(" BPM:"); Serial.print(beatAvg);
      Serial.print(" SpO2:"); Serial.print(spo2, 1);
      Serial.print(" BP:");
      if (millis() - lastBPTime < bpTimeout) Serial.println(lastBP);
      else Serial.println("Stale");
    }

    updateDisplayPeriodically();
  }
}

// ---------- BP helper ----------
void readBP() {
  if (Serial2.available() > 0) {
    String instr = Serial2.readStringUntil('\n');
    instr.trim();

    // BP data from cuff looks like "120/80"
    if (instr.length() > 0 && instr != "off" && instr != "OFF") {
      // Valid cuff BP => use it
      lastBP = instr;
      lastBPTime = millis();
      Serial.println("BP updated from cuff: " + lastBP);
      return;
    }
  }

  // -------- If cuff did NOT send BP in timeout, use PPG-derived fallback --------
  if (millis() - lastBPTime > bpTimeout) {
    int sysVal = sysPeak;   // waveform systolic peak
    int diaVal = diaPeak;   // waveform diastolic peak

    // Build fallback BP format "SYS/DIA"
    lastBP = String(sysVal) + "/" + String(diaVal);
    // Do NOT update lastBPTime — so that cuff data overrides when available

    Serial.println(lastBP);
  }
}


// ---------- Display ----------
void updateDisplayPeriodically() {
  static unsigned long lastDisplay = 0;
  if (millis() - lastDisplay < 500) return;
  lastDisplay = millis();

  if (display.width() > 0) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.print("User: "); display.println(currentUser);
    display.setCursor(0, 8);
    display.print("BPM: "); display.println(beatAvg);
    display.setCursor(0, 16);
    display.print("SpO2: "); display.print(spo2, 1); display.println(" %");
    display.setCursor(0, 24);
    display.print("BP: ");
    if (millis() - lastBPTime < bpTimeout) display.println(lastBP);
    else display.println("Stale");

    // New: show glucose on same screen (option A)
    display.setCursor(80, 0);
    display.setTextSize(1);
    display.print("GLU:");
    display.setCursor(80, 10);
    display.setTextSize(1);
    display.println(lastGlucose);

    display.display();
  }

  unsigned long t = millis();
  uint32_t lastIR = 0, lastRed = 0;
  if (samplesFilled > 0) {
    int idx = (bufHead - 1 + BUFFER_SIZE) % BUFFER_SIZE;
    lastIR = irBuffer[idx];
    lastRed = redBuffer[idx];
  }
  appendLogRow(t, lastIR, lastRed, lastRawSpO2, spo2, (int)beatsPerMinute, beatAvg, lastBP);
}

// ---------- htmlPage() (dashboard) ----------
String htmlPage() {
  const char* html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0"/>
  <title>Vitals Monitor</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    body { font-family: 'Segoe UI', sans-serif; background: linear-gradient(to right, #1f1f1f, #2c3e50); color: #fff; text-align: center; padding: 20px; margin: 0; }
    h1 { font-size: 2.5em; margin-bottom: 10px; color: #00bcd4; }
    .status { font-size: 1.2em; margin-bottom: 20px; padding: 10px; border-radius: 8px; display: inline-block; }
    .status.normal { background: #2e7d32; color: #c8e6c9; }
    .status.alert { background: #c62828; color: #ffccbc; }
    .user-box { margin-bottom: 15px; }
    input, button { padding: 8px 12px; border-radius: 6px; border: none; margin: 5px; font-size: 1em; }
    input { background: rgba(255,255,255,0.1); color: #fff; text-align: center; width: 160px; }
    button { background: #00bcd4; color: white; cursor: pointer; }
    button:hover { background: #0097a7; }
    .vitals { display: flex; flex-wrap: wrap; justify-content: center; gap: 15px; margin-bottom: 20px; }
    .card { background: rgba(255, 255, 255, 0.05); border-radius: 15px; padding: 20px; width: 150px; backdrop-filter: blur(10px); box-shadow: 0 4px 20px rgba(0,0,0,0.3); }
    .card h2 { margin-bottom: 10px; font-weight: 500; color: #ccc; }
    .value { font-size: 2.5em; font-weight: bold; color: #00e676; }
    .value.alert { color: #ff5252; }
    canvas { max-width: 600px; margin: 20px auto; background: #fff; border-radius: 10px; padding: 10px; }
    @media (max-width: 600px) { .card { width: 100%; } canvas { width: 100%; } }
  </style>
</head>
<body>
  <h1>Vitals Dashboard</h1>

  <div class="user-box">
    <input id="uid" placeholder="Enter User ID">
    <button onclick="setUser()">Set User</button>
    <p>Current User: <b><span id="currentUser">UNKNOWN</span></b></p>
  </div>

  <div id="status" class="status">Loading status...</div>

  <div class="vitals">
    <div class="card"><h2>BPM</h2><div id="bpm" class="value">--</div></div>
    <div class="card"><h2>SpO₂</h2><div id="spo2" class="value">--</div></div>
    <div class="card"><h2>BP</h2><div id="bp" class="value">--</div></div>
    <div class="card"><h2>Glucose</h2><div id="glucose" class="value">--</div></div>
  </div>

  <canvas id="bpmChart"></canvas>
  <canvas id="spo2Chart"></canvas>

  <button onclick="triggerBP()">Trigger BP Sensor</button>

  <script>
    let currentUser = "UNKNOWN";
    async function setUser() {
      const user = document.getElementById('uid').value.trim();
      if (!user) return alert("Enter a user ID!");
      await fetch('/setUser?user=' + encodeURIComponent(user));
      currentUser = user;
      document.getElementById('currentUser').textContent = user;
    }

    const bpmData = [], spo2Data = [], labels = [];
    const bpmChart = new Chart(document.getElementById("bpmChart"), {
      type: 'line',
      data: { labels: labels, datasets: [{ label: 'BPM', data: bpmData, borderColor: '#00e676', backgroundColor: 'rgba(0,230,118,0.1)', tension: 0.3 }] },
      options: { scales: { y: { beginAtZero: true } }, plugins: { legend: { display: true } } }
    });

    const spo2Chart = new Chart(document.getElementById("spo2Chart"), {
      type: 'line',
      data: { labels: labels, datasets: [{ label: 'SpO₂ (%)', data: spo2Data, borderColor: '#2196f3', backgroundColor: 'rgba(33,150,243,0.1)', tension: 0.3 }] },
      options: { scales: { y: { beginAtZero: true, max: 100 } }, plugins: { legend: { display: true } } }
    });

    async function fetchVitals() {
      try {
        const res = await fetch('/vitals');
        const data = await res.json();

        const bpm = parseInt(data.bpm);
        const spo2 = parseFloat(data.spo2);
        const bp = data.bp || "--";
        const glucose = data.glucose_range || "--";

        document.getElementById("bpm").textContent = isNaN(bpm) ? "--" : bpm;
        document.getElementById("spo2").textContent = isNaN(spo2) ? "--" : spo2.toFixed(1) + ' %';
        document.getElementById("bp").textContent = bp;
        document.getElementById("glucose").textContent = glucose;

        document.getElementById("bpm").className = (bpm && (bpm < 50 || bpm > 120)) ? "value alert" : "value";
        document.getElementById("spo2").className = (spo2 && spo2 < 90) ? "value alert" : "value";
        document.getElementById("glucose").className = (glucose && glucose !== "--" && glucose !== "NORMAL") ? "value alert" : "value";

        if (!isNaN(bpm) && !isNaN(spo2)) {
          const time = new Date().toLocaleTimeString();
          bpmData.push(bpm);
          spo2Data.push(spo2);
          labels.push(time);
          if (bpmData.length > 20) { bpmData.shift(); spo2Data.shift(); labels.shift(); }
          bpmChart.update();
          spo2Chart.update();
        }

        const statusEl = document.getElementById("status");
        if ((bpm && (bpm < 50 || bpm > 120)) || (spo2 && spo2 < 90)) {
          statusEl.textContent = "⚠️ Alert: Vitals out of range";
          statusEl.className = "status alert";
        } else {
          statusEl.textContent = "✅ Vitals Normal";
          statusEl.className = "status normal";
        }
      } catch (err) {
        console.error("Vitals fetch failed:", err);
        const s = document.getElementById("status");
        s.textContent = "❌ Error fetching vitals";
        s.className = "status alert";
      }
    }

    function triggerBP() {
      fetch("/startBP")
        .then(res => { if (res.ok) alert("BP sensor triggered"); else alert("Failed to trigger BP"); })
        .catch(() => alert("Error triggering BP"));
    }

    fetchVitals();
    setInterval(fetchVitals, 2000);
  </script>
</body>
</html>
)rawliteral";
  return String(html);
}


