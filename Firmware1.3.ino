/***********************************************************
 * TIMSON Desktop Pet – FINAL RELEASE v1.2
 * 
 * - 100k/100k voltage divider (GPIO34)
 * - Servos on GPIO4 (R) & GPIO16 (L)
 * - Petting mode: wide 130° sweep (25° ↔ 155°)
 * - Deep sleep on GPIO12 long press (>10s)
 * - Config portal password: "timson123"
 * - Weather, custom message, battery, animated faces
 * - Vibrant web dashboard with editable city & API key
 ***********************************************************/

#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <ESP32Servo.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include <esp_sleep.h>

// ------------------------------------------------------------
// 1. BITMAPS (Wi‑Fi icon for dashboard)
// ------------------------------------------------------------
static const unsigned char wifi_icon[] PROGMEM = {
  0x00,0x00,0x0f,0x80,0x30,0x60,0x40,0x10,0x03,0x00,0x0c,0x00,
  0x10,0x00,0x01,0x00,0x01,0x00,0x02,0x00,0x02,0x00,0x00,0x00
};

// ------------------------------------------------------------
// 2. PIN DEFINITIONS
// ------------------------------------------------------------
#define OLED_SDA  21
#define OLED_SCL  22
#define SERVO_R   4        // right hand
#define SERVO_L   16       // left hand
#define TOUCH1_PIN 12      // INFO / SLEEP trigger / deep sleep wake
#define TOUCH2_PIN 13      // PET / SLEEP / CUSTOM
#define BAT_ADC    34      // voltage divider input

// Voltage divider resistors (updated to 100k/100k)
const float R1 = 100000.0;   // 100kΩ
const float R2 = 100000.0;   // 100kΩ
const float ADC_MAX = 4095.0;
const float VREF = 3.3;

const int TOUCH_THRESHOLD = 600;
const unsigned long SHORT_PRESS  = 1000;    // <1s = petting
const unsigned long MID_PRESS    = 3000;    // 1-3s = sleep, >3s = custom
const unsigned long SLEEP_PRESS  = 10000;   // >10s = deep sleep (GPIO12)

// Servo angles for 130° sweep
const int SERVO_CENTER = 90;           // neutral / idle
const int PETTING_AMPLITUDE = 65;      // half of total sweep (130° / 2)
const int SERVO_UP_R = 155;            // right hand up
const int SERVO_UP_L = 25;             // left hand up
const int SERVO_DOWN = 90;             // base down position (other modes)

// Petting animation period (ms)
const unsigned long PETTING_ANIM_PERIOD = 2000;

// ------------------------------------------------------------
// 3. GLOBAL OBJECTS & STATE
// ------------------------------------------------------------
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
Servo servoRight, servoLeft;
WebServer server(80);
Preferences prefs;

enum Mode { BOOT, IDLE, INFO, PETTING, SLEEP, CUSTOM_MSG };
Mode currentMode = BOOT;

String weatherStr = "No data";
float batteryVoltage = 0;
int batteryPercent = 100;
unsigned long lastWeatherFetch = 0;
const unsigned long weatherInterval = 600000;   // 10 minutes

// Face animation
int pupilX = 0, pupilY = 0;
int eyeRadius = 14, pupilRadius = 5;
bool eyesClosed = false;
unsigned long blinkTimer = 0;
const unsigned long blinkInterval = 3000;
int blinkPhase = 0;

// Custom message
String customMessage = "";

// Petting servo animation
unsigned long pettingAnimStart = 0;

// ------------------------------------------------------------
// 4. FUNCTION PROTOTYPES
// ------------------------------------------------------------
void setupWiFi();
void startMDNS();
void setupWebServer();
void handleStatus();
void handleWeatherSettings();
void handleMessageGet();
void handleMessageSet();
void setupServos();
float readBatteryVoltage();
int batteryPercentFromVoltage(float v);
void handleTouch();
void updateDisplay();
void drawBatteryIcon(int x, int y, int w, int h, int pct);
void drawFace(String emotion, int cx1, int cx2, int cy, int eyeRad, int pupRad);
void animateFace();
void moveServos(Mode mode);
void fetchWeather();
void enterDeepSleep();

// ------------------------------------------------------------
// 5. SETUP
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  u8g2.begin();
  u8g2.setContrast(255);

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TOUCHPAD) {
    Serial.println("Woke up from deep sleep!");
  }

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_logisoso28_tr);
  u8g2.drawStr(10, 45, "TIMSON");
  u8g2.sendBuffer();
  delay(2000);

  setupServos();
  prefs.begin("timson", false);
  customMessage = prefs.getString("custom_msg", "");

  setupWiFi();
  delay(500);
  startMDNS();
  setupWebServer();

  configTime(19800, 0, "pool.ntp.org", "time.nist.gov"); // IST

  fetchWeather();
  lastWeatherFetch = millis();
  currentMode = INFO;
}

// ------------------------------------------------------------
// 6. LOOP
// ------------------------------------------------------------
void loop() {
  static unsigned long lastTouch = 0, lastBattery = 0;
  server.handleClient();

  if (millis() - lastTouch > 30) { handleTouch(); lastTouch = millis(); }
  if (millis() - lastBattery > 30000) {
    batteryVoltage = readBatteryVoltage();
    batteryPercent = batteryPercentFromVoltage(batteryVoltage);
    lastBattery = millis();
  }
  if (millis() - lastWeatherFetch > weatherInterval && WiFi.status() == WL_CONNECTED) {
    fetchWeather(); lastWeatherFetch = millis();
  }

  animateFace();
  updateDisplay();
  moveServos(currentMode);
}

// ------------------------------------------------------------
// 7. WIFI MANAGER (with portal password)
// ------------------------------------------------------------
void setupWiFi() {
  String savedKey = prefs.getString("owm_key", "");
  String savedCity = prefs.getString("owm_city", "");

  WiFiManager wm;

  WiFiManagerParameter weather_key("weather", "Weather API Key", savedKey.c_str(), 40);
  WiFiManagerParameter weather_city("city", "City (e.g. Idukki,IN)", savedCity.c_str(), 40);
  wm.addParameter(&weather_key);
  wm.addParameter(&weather_city);

  // AP "TIMSON-AP" with password "timson123"
  if (!wm.autoConnect("TIMSON-AP", "timson123")) {
    Serial.println("Failed to connect to Wi‑Fi or start config portal");
    ESP.restart();
  }

  prefs.putString("owm_key", weather_key.getValue());
  prefs.putString("owm_city", weather_city.getValue());
}

void startMDNS() {
  if (MDNS.begin("timson")) Serial.println("http://timson.local");
}

// ------------------------------------------------------------
// 8. WEB SERVER (vibrant UI + city fix + year)
// ------------------------------------------------------------
void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8"><title>TIMSON</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta http-equiv="Cache-Control" content="no-cache, no-store, must-revalidate" />
  <meta http-equiv="Pragma" content="no-cache" /><meta http-equiv="Expires" content="0" />
  <style>
    * { margin:0; padding:0; box-sizing:border-box; }
    body { font-family:'Poppins','Segoe UI',sans-serif; background:linear-gradient(135deg,#0f0c29,#302b63,#24243e); color:white; min-height:100vh; display:flex; flex-direction:column; align-items:center; padding:20px; }
    .header { text-align:center; margin-bottom:20px; }
    .header h1 { font-size:2.5em; background:linear-gradient(90deg,#ff6ec4,#7873f5); -webkit-background-clip:text; -webkit-text-fill-color:transparent; text-shadow:0 0 20px rgba(120,115,245,0.5); }
    .container { display:flex; flex-wrap:wrap; justify-content:center; gap:20px; max-width:900px; width:100%; }
    .card { background:rgba(255,255,255,0.05); backdrop-filter:blur(10px); border-radius:20px; padding:25px; flex:1 1 280px; max-width:400px; box-shadow:0 8px 32px rgba(0,0,0,0.3); border:1px solid rgba(255,255,255,0.1); }
    .card h2 { font-size:1.4em; margin-bottom:15px; color:#ff8c00; text-shadow:0 0 10px #ff8c00; }
    .emoji { font-size:60px; margin:10px 0; }
    .datetime { font-size:14px; color:#ccc; margin:5px 0; }
    .battery { font-size:18px; color:#4caf50; }
    .low { color:#f44336; }
    .weather { font-size:16px; color:#fff; margin-top:10px; }
    button { background:linear-gradient(90deg,#ff512f,#dd2476); border:none; color:white; padding:10px 20px; border-radius:25px; cursor:pointer; font-size:16px; margin-top:15px; transition:transform 0.2s,box-shadow 0.2s; box-shadow:0 4px 15px rgba(255,81,47,0.4); }
    button:hover { transform:scale(1.05); box-shadow:0 6px 20px rgba(255,81,47,0.6); }
    button:disabled { background:#555; box-shadow:none; cursor:not-allowed; }
    input, textarea { width:100%; padding:10px; margin:8px 0; border-radius:10px; border:1px solid rgba(255,255,255,0.2); background:rgba(255,255,255,0.1); color:white; font-size:14px; backdrop-filter:blur(5px); }
    input::placeholder, textarea::placeholder { color:#aaa; }
    textarea { resize:vertical; }
    label { display:block; margin-top:12px; font-weight:bold; color:#ffcc00; }
    .footer { margin-top:30px; text-align:center; font-size:14px; color:#aaa; }
    .footer span { color:#ff6ec4; font-weight:bold; }
    @media (max-width:600px) { .container { flex-direction:column; align-items:center; } .card { max-width:100%; } }
  </style>
</head>
<body>
  <div class="header">
    <h1>🤖 TIMSON</h1>
    <p style="color:#aaa;">Your friendly desktop companion</p>
  </div>
  <div class="container">
    <div class="card">
      <h2>📊 Live Status</h2>
      <div id="emotion" class="emoji">😊</div>
      <div id="emotionName" style="font-size:18px; margin-bottom:5px;">Idle</div>
      <div id="datetime" class="datetime">--</div>
      <div id="battery" class="battery">🔋 --%</div>
      <div id="weather" class="weather">--</div>
    </div>
    <div class="card">
      <h2>⚙️ Weather Settings</h2>
      <label>API Key</label>
      <input type="text" id="apiKey" placeholder="Key saved (enter to change)">
      <label>City</label>
      <input type="text" id="city" placeholder="Idukki,IN">
      <button onclick="saveWeatherSettings()">Save Weather</button>
      <p id="saveStatus" style="color:#4caf50; margin-top:8px;"></p>
    </div>
    <div class="card">
      <h2>✉️ Custom Message</h2>
      <label>Your Message (hold touch13 &gt;3s to show)</label>
      <textarea id="customMsg" rows="3" placeholder="Enter message..."></textarea>
      <button onclick="saveCustomMessage()">Save Message</button>
      <p id="msgStatus" style="color:#4caf50; margin-top:8px;"></p>
    </div>
  </div>
  <div class="footer">
    Made by <span>Akshay</span> with the help of <span>Deep Seek</span>
  </div>
  <script>
    function updateData() {
      fetch('/api/status?t='+new Date().getTime())
        .then(r=>r.json())
        .then(d=>{
          document.getElementById('emotion').textContent = d.emotion_icon || '😊';
          document.getElementById('emotionName').textContent = d.emotion_name || 'Idle';
          document.getElementById('datetime').textContent = d.datetime || '';
          var bat = document.getElementById('battery');
          bat.textContent = '🔋 ' + (d.battery != null ? d.battery : '--') + '%';
          bat.className = d.battery < 20 ? 'battery low' : 'battery';
          document.getElementById('weather').textContent = d.weather || '--';
          var keyInput = document.getElementById('apiKey');
          if (d.key_set) { keyInput.placeholder = "Key saved (enter to change)"; keyInput.value = ""; }
          else { keyInput.placeholder = "Enter OpenWeatherMap API key"; }
          var cityInput = document.getElementById('city');
          if (d.weather_city) {
            cityInput.placeholder = d.weather_city;
            if (!cityInput.value) cityInput.value = d.weather_city;
          }
        });
    }
    function loadCustomMessage() {
      fetch('/api/message').then(r=>r.json()).then(d=>{
        document.getElementById('customMsg').value = d.message || '';
      });
    }
    updateData(); loadCustomMessage(); setInterval(updateData, 2000);
    function saveWeatherSettings() {
      var key = document.getElementById('apiKey').value.trim();
      var city = document.getElementById('city').value.trim();
      var body = '';
      if (key) body += 'key=' + encodeURIComponent(key);
      if (city) { if (body) body += '&'; body += 'city=' + encodeURIComponent(city); }
      if (!body) { document.getElementById('saveStatus').textContent = "Nothing to save"; return; }
      fetch('/api/weather/settings', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:body })
        .then(r=>r.text()).then(txt=>{ document.getElementById('saveStatus').textContent = txt; });
    }
    function saveCustomMessage() {
      var msg = document.getElementById('customMsg').value.trim();
      fetch('/api/message', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'msg='+encodeURIComponent(msg) })
        .then(r=>r.text()).then(txt=>{ document.getElementById('msgStatus').textContent = txt; });
    }
  </script>
</body>
</html>
    )rawliteral");
  });

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/weather/settings", HTTP_POST, handleWeatherSettings);
  server.on("/api/message", HTTP_GET, handleMessageGet);
  server.on("/api/message", HTTP_POST, handleMessageSet);
  server.begin();
}

void handleStatus() {
  DynamicJsonDocument doc(512);
  String emoji="😊",name="Idle";
  if(currentMode==PETTING){emoji="❤️";name="Petting";}
  else if(currentMode==SLEEP){emoji="😴";name="Sleep";}
  else if(currentMode==INFO){emoji="📅";name="Info";}
  else if(currentMode==CUSTOM_MSG){emoji="📝";name="Custom Msg";}
  doc["emotion_icon"]=emoji; doc["emotion_name"]=name;

  struct tm ti;
  char dtStr[30];
  if(getLocalTime(&ti)){
    strftime(dtStr, 30, "%a, %d %b %Y  %I:%M %p", &ti);
    doc["datetime"] = dtStr;
  } else { doc["datetime"] = "Time not synced"; }

  doc["battery"]=batteryPercent;
  doc["weather"]=weatherStr;
  String storedKey = prefs.getString("owm_key","");
  doc["key_set"] = (storedKey.length() > 0);
  doc["weather_city"] = prefs.getString("owm_city","");
  String out; serializeJson(doc,out);
  server.sendHeader("Cache-Control","no-cache, no-store, must-revalidate");
  server.send(200,"application/json",out);
}

void handleWeatherSettings() {
  String key = server.hasArg("key") ? server.arg("key") : "";
  String city = server.hasArg("city") ? server.arg("city") : "";
  bool updated = false;
  if (key.length() > 0) { prefs.putString("owm_key", key); updated = true; }
  if (city.length() > 0) { prefs.putString("owm_city", city); updated = true; }
  if (updated) { fetchWeather(); lastWeatherFetch = millis(); server.send(200, "text/plain", "Saved!"); }
  else { server.send(200, "text/plain", "Nothing to save"); }
}

void handleMessageGet() {
  DynamicJsonDocument doc(256);
  doc["message"] = customMessage;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleMessageSet() {
  if (server.hasArg("msg")) {
    customMessage = server.arg("msg");
    prefs.putString("custom_msg", customMessage);
    server.send(200, "text/plain", "Message saved!");
  } else { server.send(400, "text/plain", "Missing msg parameter"); }
}

// ------------------------------------------------------------
// 9. SERVO SETUP
// ------------------------------------------------------------
void setupServos() {
  servoRight.attach(SERVO_R);
  servoLeft.attach(SERVO_L);
  moveServos(IDLE);
}

// ------------------------------------------------------------
// 10. BATTERY (100k/100k divider)
// ------------------------------------------------------------
float readBatteryVoltage() {
  int raw = analogRead(BAT_ADC);
  float vout = (raw / ADC_MAX) * VREF;
  return vout / (R2 / (R1 + R2));   // works for any resistor ratio
}

int batteryPercentFromVoltage(float v) {
  // Li‑Po: 4.2V full, 3.3V empty
  return constrain(map((int)(v * 100), 330, 420, 0, 100), 0, 100);
}

// ------------------------------------------------------------
// 11. TOUCH + DEEP SLEEP
// ------------------------------------------------------------
void handleTouch() {
  static bool t1last=false, t2last=false;
  static unsigned long t1start=0, t2start=0;
  int t1=touchRead(TOUCH1_PIN),t2=touchRead(TOUCH2_PIN);
  bool t1now=(t1<TOUCH_THRESHOLD),t2now=(t2<TOUCH_THRESHOLD);

  static unsigned long lastDebug=0;
  if(millis()-lastDebug>500){
    Serial.printf("Touch: T1=%d T2=%d (thresh=%d)\n",t1,t2,TOUCH_THRESHOLD);
    lastDebug=millis();
  }

  // T1 (GPIO12)
  if(t1now && !t1last) t1start = millis();
  if(!t1now && t1last){
    unsigned long dur = millis() - t1start;
    if(dur >= SLEEP_PRESS) {
      enterDeepSleep();
    } else if(currentMode == CUSTOM_MSG) {
      currentMode = INFO;
    } else {
      currentMode = (dur < SHORT_PRESS) ? INFO : IDLE;
    }
  }

  // T2 (GPIO13)
  if(t2now && !t2last) t2start = millis();
  if(!t2now && t2last){
    unsigned long dur = millis() - t2start;
    if (dur < SHORT_PRESS) {
      currentMode = PETTING;
      pettingAnimStart = 0;   // start petting animation
    } else if (dur < MID_PRESS) {
      currentMode = SLEEP;
    } else {
      currentMode = CUSTOM_MSG;
    }
  }

  t1last = t1now; t2last = t2now;
}

// ------------------------------------------------------------
// 12. DEEP SLEEP
// ------------------------------------------------------------
void enterDeepSleep() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(10, 30, "Sleeping...");
  u8g2.drawStr(5, 50, "Touch GPIO12 to wake");
  u8g2.sendBuffer();
  delay(1000);

  u8g2.setPowerSave(true);
  servoRight.detach();
  servoLeft.detach();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  touchSleepWakeUpEnable(TOUCH1_PIN, TOUCH_THRESHOLD);
  esp_deep_sleep_start();
}

// ------------------------------------------------------------
// 13. DISPLAY (OLED)
// ------------------------------------------------------------
void updateDisplay() {
  u8g2.clearBuffer();

  if (currentMode == BOOT) {
    u8g2.setFont(u8g2_font_logisoso28_tr);
    u8g2.drawStr(10, 45, "TIMSON");
  }
  else if (currentMode == INFO || currentMode == IDLE) {
    drawBatteryIcon(85, 0, 18, 8, batteryPercent);
    struct tm ti;
    if (getLocalTime(&ti)) {
      char timeStr[12];
      strftime(timeStr, 12, "%I:%M %p", &ti);
      u8g2.setFont(u8g2_font_logisoso24_tr);
      u8g2.drawStr(0, 40, timeStr);
      char dateStr[20];
      strftime(dateStr, 20, "%a %d %b %Y", &ti);
      u8g2.setFont(u8g2_font_6x13_tr);
      u8g2.drawStr(2, 54, dateStr);
    } else {
      u8g2.setFont(u8g2_font_logisoso24_tr);
      u8g2.drawStr(0, 40, "--:-- --");
    }
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(2, 63, weatherStr.c_str());
  }
  else if (currentMode == PETTING) {
    drawFace("happy", 35, 93, 32, 14, 5);
  }
  else if (currentMode == SLEEP) {
    drawFace("sleepy", 35, 93, 32, 14, 5);
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(100, 55, "Zzz");
  }
  else if (currentMode == CUSTOM_MSG) {
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0, 10, "Message:");
    String msg = customMessage.length() > 0 ? customMessage : "No message set.";
    int maxChars = 21, start = 0, line = 0;
    while (start < msg.length() && line < 4) {
      int len = msg.length() - start;
      if (len > maxChars) len = maxChars;
      u8g2.drawStr(0, 22 + line*12, msg.substring(start, start+len).c_str());
      start += len; line++;
    }
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(0, 63, "Touch GPIO12 to exit");
  }
  u8g2.sendBuffer();
}

void drawBatteryIcon(int x, int y, int w, int h, int pct) {
  u8g2.drawFrame(x, y, w, h);
  u8g2.drawBox(x + w, y + 2, 2, h - 4);
  int fillW = map(pct, 0, 100, 0, w);
  if (fillW > 0) u8g2.drawBox(x, y, fillW, h);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(x + w + 4, y + 7, (String(pct) + "%").c_str());
}

// ------------------------------------------------------------
// 14. FACE DRAWING
// ------------------------------------------------------------
void drawFace(String emotion, int cx1, int cx2, int cy, int eyeRad, int pupRad) {
  int px1 = cx1 + pupilX, py1 = cy + pupilY;
  int px2 = cx2 + pupilX, py2 = cy + pupilY;

  if (eyesClosed) {
    u8g2.drawLine(cx1 - eyeRad, cy, cx1 + eyeRad, cy);
    u8g2.drawLine(cx2 - eyeRad, cy, cx2 + eyeRad, cy);
  } else {
    u8g2.drawCircle(cx1, cy, eyeRad);
    u8g2.drawCircle(cx2, cy, eyeRad);
    u8g2.drawDisc(px1, py1, pupRad);
    u8g2.drawDisc(px2, py2, pupRad);
  }

  if (emotion == "angry") {
    u8g2.drawLine(cx1 - 12, cy - 20, cx1 + 6, cy - 12);
    u8g2.drawLine(cx2 - 6, cy - 12, cx2 + 12, cy - 20);
  } else if (emotion == "surprised" || emotion == "sleepy") {
    u8g2.drawLine(cx1 - 12, cy - 24, cx1 + 12, cy - 24);
    u8g2.drawLine(cx2 - 12, cy - 24, cx2 + 12, cy - 24);
  } else {
    u8g2.drawLine(cx1 - 12, cy - 18, cx1 + 12, cy - 18);
    u8g2.drawLine(cx2 - 12, cy - 18, cx2 + 12, cy - 18);
  }

  int mouthY = cy + 22;
  if (emotion == "happy") {
    for (int x = -14; x <= 14; x++) u8g2.drawPixel(64 + x, mouthY + 4 * sin(radians(x * 9)));
  } else if (emotion == "sad") {
    for (int x = -14; x <= 14; x++) u8g2.drawPixel(64 + x, mouthY - 4 * sin(radians(x * 9)));
  } else if (emotion == "surprised") {
    u8g2.drawCircle(64, mouthY, 5);
  } else {
    u8g2.drawLine(58, mouthY, 70, mouthY);
  }
}

// ------------------------------------------------------------
// 15. ANIMATIONS (face + servos)
// ------------------------------------------------------------
void animateFace() {
  // Blink
  if (millis() - blinkTimer > blinkInterval) {
    blinkTimer = millis();
    eyesClosed = true;
    blinkPhase = 1;
  }
  if (blinkPhase == 1 && millis() - blinkTimer > 150) {
    eyesClosed = false;
    blinkPhase = 0;
  }

  // Eye roll (petting)
  if (currentMode == PETTING) {
    static unsigned long rollStart = 0;
    static int rollAngle = 0;
    if (!rollStart || millis() - rollStart > 2000) {
      rollStart = millis();
      rollAngle = random(0, 360);
    }
    float progress = (millis() - rollStart) / 2000.0;
    int angle = rollAngle + progress * 360;
    pupilX = cos(radians(angle)) * 4;
    pupilY = sin(radians(angle)) * 4;
  } else if (currentMode == SLEEP) {
    eyesClosed = true;
    pupilX = 0; pupilY = 0;
  } else {
    pupilX = 0; pupilY = 0;
  }
}

// ------------------------------------------------------------
// 16. SERVO MOVEMENT (130° sweep in PETTING)
// ------------------------------------------------------------
void moveServos(Mode mode) {
  switch (mode) {
    case PETTING: {
      // Smooth triangle wave over 130° sweep
      if (pettingAnimStart == 0) pettingAnimStart = millis();
      unsigned long elapsed = (millis() - pettingAnimStart) % PETTING_ANIM_PERIOD;
      float phase = (float)elapsed / PETTING_ANIM_PERIOD;          // 0.0 → 1.0
      float triangle;
      if (phase < 0.5) triangle = phase * 2.0;                    // 0 → 1
      else             triangle = (1.0 - phase) * 2.0;            // 1 → 0

      // Right hand: 25° (down) → 155° (up) and back
      int rightAngle = SERVO_CENTER - PETTING_AMPLITUDE + (2 * PETTING_AMPLITUDE) * triangle;
      // Left hand: 155° (down) → 25° (up) and back (mirror)
      int leftAngle  = SERVO_CENTER + PETTING_AMPLITUDE - (2 * PETTING_AMPLITUDE) * triangle;

      servoRight.write(rightAngle);
      servoLeft.write(leftAngle);
      break;
    }
    case SLEEP:
    case CUSTOM_MSG:
      servoRight.write(SERVO_DOWN);
      servoLeft.write(SERVO_DOWN);
      break;

    case INFO:
      servoRight.write(SERVO_UP_R);
      servoLeft.write(SERVO_UP_L);
      break;

    default:
      servoRight.write(SERVO_DOWN);
      servoLeft.write(SERVO_DOWN);
      break;
  }
}

// ------------------------------------------------------------
// 17. WEATHER FETCH
// ------------------------------------------------------------
void fetchWeather() {
  String apiKey = prefs.getString("owm_key", "");
  if (apiKey.isEmpty()) { weatherStr = "No API key"; return; }
  String city = prefs.getString("owm_city", "");
  if (city.isEmpty()) { weatherStr = "Set city"; return; }

  String url = "http://api.openweathermap.org/data/2.5/weather?q=" + city + "&units=metric&appid=" + apiKey;
  HTTPClient http; http.begin(url);
  if (http.GET() == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(1024); deserializeJson(doc, payload);
    float temp = doc["main"]["temp"];
    String cond = doc["weather"][0]["main"];
    weatherStr = city + ": " + String(temp, 1) + "°C, " + cond;
  } else { weatherStr = city + ": Error"; }
  http.end();
}