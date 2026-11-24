// Claude Buddy: M5StickC Plus desk buddy that shows what Claude Code is doing.
// Messages arrive on an ntfy.sh topic (or USB serial) as plain words:
//   working | done [usedK/maxK] | input [usedK/maxK] | idle      Claude Code state, with context size
//   alarm | alarm-ok | deploy-ok | deploy-fail                  overlays from AWS alarms / deploys
//   ota <hmac-sha256>  (+ attachment)                           signed firmware update over WiFi
// Button A or a shake = "got it". Face-down = mute. Hold B (side) 3 s = redo WiFi setup.
#include <M5Unified.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "mbedtls/md.h"
#if __has_include("ota_key.h")
#include "ota_key.h"  // #define OTA_KEY "..."; gitignored. Without it, OTA is off.
#endif

#define DEFAULT_TOPIC "claude-buddy-CHANGE-ME"  // placeholder: the stick stays offline until you set your own
#define AP_NAME "ClaudeBuddy-Setup"
static const int LED_PIN = 10;           // red LED, active low
static const float SHAKE_G = 1.4f;       // calibration: extra g that counts as a shake (1.0 = lying still)
static const float FACE_DOWN_Z = -0.8f;  // calibration: accel z when screen faces the desk

enum State { IDLE, WORKING, DONE, NEEDS_INPUT };
enum Overlay { OV_NONE, OV_ALARM, OV_DEPLOY_FAIL, OV_DEPLOY_OK };
static State state = IDLE;
static Overlay overlay = OV_NONE;
static uint32_t overlaySince = 0;
static int ctxK = 0, ctxMaxK = 0;  // context window use, from the done/input message

static Preferences prefs;
static char topic[64] = DEFAULT_TOPIC;
static WiFiClientSecure net;
static String lastId = "latest";  // first connect replays the newest message so we start in the right state
static String line;
static bool headersDone = false, streamOk = false, paramsSaved = false;
static uint32_t lastData = 0, nextConnect = 0;

static bool faceDown = false, onUsb = true;
static int battery = 100;

static M5Canvas cv(&M5.Display);
static int W, H;

static void message(const char *a, const char *b = "", const char *c = "");

// ---------- sound ----------
static void beep(int freq, int ms) {
  if (faceDown) return;
  M5.Speaker.tone(freq, ms);
  delay(ms + 30);
}

// ---------- state ----------
static void setState(State s) {
  if (s == state) return;
  state = s;
  if (s == DONE) { beep(1800, 80); beep(2400, 120); }
  if (s == NEEDS_INPUT) { beep(1200, 150); beep(1200, 150); }
}

static void setOverlay(Overlay o) {
  overlay = o;
  overlaySince = millis();
  if (o == OV_ALARM) for (int i = 0; i < 3; i++) { beep(900, 120); beep(600, 120); }
  if (o == OV_DEPLOY_FAIL) { beep(700, 200); beep(500, 300); }
  if (o == OV_DEPLOY_OK) { beep(1500, 60); beep(2000, 60); beep(2600, 90); }
}

static void acknowledge() {
  if (overlay != OV_NONE) overlay = OV_NONE;
  else setState(IDLE);
}

static void doOta(const String &url, String mac);

static void handleMessage(String msg, const String &attachmentUrl = "") {
  msg.trim();
  int sp = msg.indexOf(' ');
  String w = sp > 0 ? msg.substring(0, sp) : msg, arg = sp > 0 ? msg.substring(sp + 1) : "";
  w.toLowerCase();
  if ((w == "done" || w == "input") && sscanf(arg.c_str(), "%d/%d", &ctxK, &ctxMaxK) != 2) ctxK = ctxMaxK = 0;

  if (w == "working" || w == "busy") setState(WORKING);
  else if (w == "done" || w == "stop") setState(DONE);
  else if (w == "input" || w == "permission") setState(NEEDS_INPUT);
  else if (w == "idle") setState(IDLE);
  else if (w == "alarm") setOverlay(OV_ALARM);
  else if (w == "alarm-ok") { if (overlay == OV_ALARM) overlay = OV_NONE; }
  else if (w == "deploy-fail") { if (overlay != OV_ALARM) setOverlay(OV_DEPLOY_FAIL); }
  else if (w == "deploy-ok") { if (overlay == OV_NONE || overlay == OV_DEPLOY_FAIL) setOverlay(OV_DEPLOY_OK); }
  else if (w == "ota") doOta(attachmentUrl, arg);
}

// ---------- sensors ----------
static void pollSensors() {
  static uint32_t lastShake = 0, lastPower = 0;
  float ax, ay, az;
  if (M5.Imu.getAccel(&ax, &ay, &az)) {
    float extra = fabsf(sqrtf(ax * ax + ay * ay + az * az) - 1.0f);
    if (extra > SHAKE_G && millis() - lastShake > 1500) { lastShake = millis(); acknowledge(); }
    faceDown = az < FACE_DOWN_Z;
  }
  if (millis() - lastPower > 5000 || !lastPower) {
    lastPower = millis();
    int vbus = M5.Power.getVBUSVoltage();
    onUsb = vbus < 0 || vbus > 4000;  // -1 = can't tell; assume USB and hide the battery icon
    int level = M5.Power.getBatteryLevel();
    if (level >= 0) {
      if (!onUsb && level < 15 && battery >= 15) beep(400, 300);  // one warning as it crosses 15 %
      battery = level;
    }
  }
}

// ---------- drawing ----------
static void eyeOpen(int cx, int cy, int r, int lookX, uint16_t fg, uint16_t bg) {
  cv.fillRoundRect(cx - r * 0.7, cy - r, r * 1.4, r * 2, r * 0.6, fg);
  cv.fillCircle(cx + lookX, cy + r * 0.2, r * 0.38, bg);
}

static void drawStatusBar(uint16_t fg) {
  cv.setFont(&fonts::Font2);
  if (ctxK > 0) {  // context window: number plus a small bar
    cv.setTextDatum(top_left);
    cv.setTextColor(fg);
    cv.drawString(String(ctxK) + "k", 4, 2);
    if (ctxMaxK > 0) {
      int bw = 40, fill = min(bw, bw * ctxK / ctxMaxK);
      cv.drawRect(4, 19, bw, 5, fg);
      cv.fillRect(4, 19, fill, 5, fg);
    }
  }
  if (!onUsb) {  // battery icon, red when low
    uint16_t c = battery < 20 ? TFT_RED : fg;
    int x = W - 40, y = 2;
    cv.drawRect(x, y, 22, 11, c);
    cv.fillRect(x + 22, y + 3, 2, 5, c);
    cv.fillRect(x + 2, y + 2, 18 * battery / 100, 7, c);
  }
  if (!streamOk) cv.fillCircle(W - 6, 6, 4, TFT_RED);  // offline dot
}

static void draw() {
  uint32_t t = millis();
  float s = t / 1000.0f;
  if (overlay == OV_DEPLOY_OK && t - overlaySince > 8000) overlay = OV_NONE;  // good news is brief

  uint16_t bg, fg = TFT_WHITE;
  const char *label;
  int face;  // 0 sleep, 1 working, 2 happy, 3 wide-eyed, 4 worried
  switch (overlay) {
    case OV_ALARM:       bg = cv.color565(200, 30, 30);  label = "lambda alarm"; face = 4; break;
    case OV_DEPLOY_FAIL: bg = cv.color565(130, 30, 100); label = "deploy failed"; face = 4; break;
    case OV_DEPLOY_OK:   bg = cv.color565(40, 150, 70);  label = "deployed!"; face = 2; break;
    default:
      switch (state) {
        case WORKING:     bg = cv.color565(20, 60, 70);   label = "working"; face = 1; break;
        case DONE:        bg = cv.color565(217, 119, 87); label = "your turn!"; face = 2; break;
        case NEEDS_INPUT: bg = cv.color565(230, 170, 30); label = "need you"; fg = TFT_BLACK; face = 3; break;
        default:          bg = cv.color565(15, 15, 35);   label = "zzz"; face = 0; break;
      }
  }
  bool urgent = overlay == OV_ALARM || overlay == OV_DEPLOY_FAIL || state == DONE || state == NEEDS_INPUT;
  M5.Display.setBrightness(faceDown ? 0 : (face == 0 ? 30 : 160));
  cv.fillScreen(bg);

  int r = H / 7;  // eye half-height
  int bounce = face == 2 ? fabsf(sinf(s * 5)) * -H / 14 : 0;
  int shake = face == 4 ? sinf(s * 30) * 2 : 0;
  int ey = H * 0.38 + bounce, ex = W * 0.18;
  int lx = W / 2 - ex + shake, rx = W / 2 + ex + shake;
  bool blink = (t % 4000) < 120;

  switch (face) {
    case 1: {
      int look = sinf(s * 2) * r * 0.35;
      if (blink) { cv.fillRect(lx - r * 0.7, ey, r * 1.4, 3, fg); cv.fillRect(rx - r * 0.7, ey, r * 1.4, 3, fg); }
      else { eyeOpen(lx, ey, r, look, fg, bg); eyeOpen(rx, ey, r, look, fg, bg); }
      cv.fillRect(W / 2 - r * 0.6, ey + r * 1.6, r * 1.2, 3, fg);  // flat concentrating mouth
      break;
    }
    case 2:  // ^ ^ eyes and a big smile
      cv.fillArc(lx, ey + r * 0.5, r, r - 4, 200, 340, fg);
      cv.fillArc(rx, ey + r * 0.5, r, r - 4, 200, 340, fg);
      cv.fillArc(W / 2, ey + r * 0.6, r * 1.3, r * 1.3 - 4, 20, 160, fg);
      break;
    case 3:  // wide round eyes, little o mouth
      cv.fillCircle(lx, ey, r, fg); cv.fillCircle(lx + r * 0.35, ey - r * 0.35, r * 0.25, bg);
      cv.fillCircle(rx, ey, r, fg); cv.fillCircle(rx + r * 0.35, ey - r * 0.35, r * 0.25, bg);
      cv.fillCircle(W / 2, ey + r * 1.7, r * 0.4, fg); cv.fillCircle(W / 2, ey + r * 1.7, r * 0.4 - 4, bg);
      break;
    case 4:  // worried: round eyes, slanted brows, frown, trembling
      cv.fillCircle(lx, ey, r * 0.8, fg); cv.fillCircle(lx, ey + r * 0.15, r * 0.35, bg);
      cv.fillCircle(rx, ey, r * 0.8, fg); cv.fillCircle(rx, ey + r * 0.15, r * 0.35, bg);
      cv.drawWideLine(lx - r, ey - r * 1.1, lx + r * 0.6, ey - r * 1.5, 3, fg);
      cv.drawWideLine(rx + r, ey - r * 1.1, rx - r * 0.6, ey - r * 1.5, 3, fg);
      cv.fillArc(W / 2 + shake, ey + r * 2.4, r, r - 4, 220, 320, fg);
      break;
    default:  // asleep: closed eyes, drifting z
      cv.fillArc(lx, ey, r, r - 3, 20, 160, fg);
      cv.fillArc(rx, ey, r, r - 3, 20, 160, fg);
      break;
  }

  cv.setTextColor(fg);
  cv.setTextDatum(bottom_center);
  cv.setFont(H >= 120 ? &fonts::FreeSansBold12pt7b : &fonts::FreeSansBold9pt7b);
  if (face == 1) {
    String dots = String("...").substring(0, (t / 400) % 4);
    cv.drawString(String(label) + dots, W / 2, H - 4);
  } else if (face == 0) {
    cv.setTextDatum(middle_left);
    cv.drawString("z", rx + r + ((t / 60) % 20) / 4, ey - r - ((t / 60) % 20) / 2);
  } else {
    cv.drawString(label, W / 2, H - 4);
  }

  drawStatusBar(fg);
  cv.pushSprite(0, 0);
  digitalWrite(LED_PIN, urgent && (t % 1000) < 500 ? LOW : HIGH);
}

static void message(const char *a, const char *b, const char *c) {
  M5.Display.setBrightness(160);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextDatum(top_left);
  M5.Display.drawString(a, 4, 4);
  M5.Display.drawString(b, 4, 24);
  M5.Display.drawString(c, 4, 44);
}

// ---------- OTA ----------
// Firmware comes as an ntfy attachment. Anyone who knows the topic can post one, so the image must carry an
// HMAC-SHA256 made with OTA_KEY (tools/ota.sh does this). Flash is written as it streams, but the new image only
// becomes bootable after the HMAC matches; otherwise Update.abort() leaves the running firmware alone.
static void doOta(const String &url, String mac) {
#ifdef OTA_KEY
  mac.trim();
  mac.toLowerCase();
  if (url.isEmpty() || mac.length() != 64) return;
  if (prefs.getString("ota", "") == mac) { Serial.println("ota: already applied or rejected"); return; }
  message("Updating firmware...", "don't unplug");
  net.stop();
  streamOk = false;

  WiFiClientSecure client;
  client.setInsecure();  // integrity comes from the HMAC, not from TLS
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, url);
  int code = http.GET(), len = http.getSize();
  if (code != 200 || len <= 0 || !Update.begin(len)) {
    Serial.printf("ota: http %d len %d\n", code, len);
    http.end();
    return;
  }
  mbedtls_md_context_t md;
  mbedtls_md_init(&md);
  mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&md, (const uint8_t *)OTA_KEY, strlen(OTA_KEY));
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  int got = 0;
  uint32_t lastByte = millis();
  Serial.printf("ota: %d bytes, heap %u max %u\n", len, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  while (got < len && millis() - lastByte < 15000) {
    int n = stream->readBytes(buf, min((int)sizeof(buf), len - got));
    if (n <= 0) continue;
    lastByte = millis();
    if ((got & 0x1FFFF) + n > 0x1FFFF) Serial.printf("ota: %d heap %u\n", got, ESP.getFreeHeap());
    mbedtls_md_hmac_update(&md, buf, n);
    if (Update.write(buf, n) != (size_t)n) break;
    got += n;
  }
  uint8_t sum[32];
  mbedtls_md_hmac_finish(&md, sum);
  mbedtls_md_free(&md);
  http.end();
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", sum[i]);

  if (got != len) {
    // ponytail: retry by rebooting. Boot replays the newest topic message, so this retries while the update is
    // still the latest message and its file hasn't expired (3 h); otherwise the stick just carries on.
    Update.abort();
    Serial.printf("ota: short download %d/%d, rebooting to retry\n", got, len);
    delay(1000);
    ESP.restart();
  }
  if (mac != hex) {
    Update.abort();
    prefs.putString("ota", mac);  // don't re-download a bad image on every boot
    Serial.println("ota: HMAC mismatch, rejected");
    message("Update rejected", "(bad signature)");
    delay(3000);
    return;
  }
  if (!Update.end(true)) { Serial.printf("ota: end failed %s\n", Update.errorString()); return; }
  prefs.putString("ota", mac);
  Serial.println("ota: ok, restarting");
  message("Updated, restarting");
  delay(1000);
  ESP.restart();
#endif
}

// ---------- wifi + ntfy ----------
static WiFiManager wm;
static WiFiManagerParameter topicParam("topic", "ntfy topic", DEFAULT_TOPIC, 63);
static bool portal = false, setupShown = false;

static void setupWifi(bool forcePortal) {
  wm.setDebugOutput(false);  // its debug log prints the saved WiFi password on serial
  wm.addParameter(&topicParam);
  wm.setSaveParamsCallback([] { paramsSaved = true; });
  wm.setSaveConfigCallback([] { paramsSaved = true; });
  wm.setConfigPortalBlocking(false);  // keep loop() (serial, faces) alive while the portal is up
  wm.setConfigPortalTimeout(300);     // portal gives up after 5 min, then we reboot and retry the saved network
  message("Connecting WiFi...", WiFi.SSID().c_str());
  portal = forcePortal ? (wm.startConfigPortal(AP_NAME), true) : !wm.autoConnect(AP_NAME);
}

static void pollPortal() {
  wm.process();
  if (paramsSaved && strlen(topicParam.getValue())) {
    paramsSaved = false;
    strlcpy(topic, topicParam.getValue(), sizeof(topic));
    prefs.putString("topic", topic);
  }
  if (WiFi.status() == WL_CONNECTED) { portal = false; Serial.printf("wifi ok %s\n", WiFi.localIP().toString().c_str()); return; }
  if (!wm.getConfigPortalActive()) ESP.restart();
  if (state == IDLE && overlay == OV_NONE && !setupShown) {
    message("WiFi setup: join network", "  " AP_NAME, "then open 192.168.4.1");
    setupShown = true;
  }
}

static void connectNtfy() {
  net.stop();
  streamOk = headersDone = false;
  line = "";
  if (WiFi.status() != WL_CONNECTED) return;
  if (strstr(topic, "CHANGE-ME")) { Serial.println("set your own ntfy topic first"); return; }
  // ponytail: TLS without cert check. Payloads are state words (worst case a spoofed face) and OTA images are
  // HMAC-checked. Pin ISRG root if that ever matters.
  net.setInsecure();
  if (!net.connect("ntfy.sh", 443, 8000)) { Serial.println("ntfy connect failed"); return; }
  // HTTP/1.0 so the stream is not chunked: one JSON object per line until the server closes.
  net.printf("GET /%s/json?since=%s HTTP/1.0\r\nHost: ntfy.sh\r\nUser-Agent: claude-buddy\r\n\r\n", topic, lastId.c_str());
  lastData = millis();
}

static void handleLine() {
  line.trim();
  if (!headersDone) {
    if (line.startsWith("HTTP/")) streamOk = line.indexOf(" 200") > 0;
    if (line.isEmpty()) headersDone = true;
    return;
  }
  if (line.isEmpty()) return;
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  if (doc["event"] == "message") {
    lastId = doc["id"].as<String>();
    Serial.printf("ntfy: %s\n", doc["message"].as<const char *>());
    handleMessage(doc["message"].as<String>(), doc["attachment"]["url"] | "");
  }
}

static void pollNtfy() {
  while (net.available()) {
    char c = net.read();
    lastData = millis();
    if (c == '\n') { handleLine(); line = ""; }
    else if (line.length() < 1024) line += c;
  }
  // ntfy sends keepalives every 45 s; silence for 2 min means the stream is dead.
  bool dead = !net.connected() || millis() - lastData > 120000;
  if (dead && millis() > nextConnect) {
    streamOk = false;
    nextConnect = millis() + 5000;
    connectNtfy();
  }
}

static void pollSerial() {
  static String in;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n') { if (in.length() < 200) in += c; continue; }
    in.trim();
    if (in.startsWith("wifi ")) {  // wifi <ssid> <password>  (set network over USB, no phone needed)
      int sp = in.indexOf(' ', 5);
      String ssid = sp > 0 ? in.substring(5, sp) : in.substring(5);
      String pass = sp > 0 ? in.substring(sp + 1) : "";
      Serial.printf("saving wifi '%s' and rebooting\n", ssid.c_str());
      WiFi.persistent(true);
      WiFi.begin(ssid.c_str(), pass.c_str());
      delay(500);
      ESP.restart();
    } else if (in.startsWith("topic ")) {
      strlcpy(topic, in.substring(6).c_str(), sizeof(topic));
      prefs.putString("topic", topic);
      Serial.printf("topic = %s\n", topic);
      connectNtfy();
    } else if (in == "shot") {  // dump the frame as raw RGB565 for a host-side screenshot
      Serial.printf("SHOT %d %d\n", W, H);
      Serial.write((const uint8_t *)cv.getBuffer(), W * H * 2);
      Serial.println();
    } else if (in == "imu") {
      float ax, ay, az;
      M5.Imu.getAccel(&ax, &ay, &az);
      Serial.printf("accel %.2f %.2f %.2f faceDown %d\n", ax, ay, az, faceDown);
    } else if (in == "status") {
      Serial.printf("wifi %s ip %s topic %s stream %s state %d overlay %d usb %d battery %d ota %s\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), topic, streamOk ? "ok" : "down",
                    state, overlay, onUsb, battery,
#ifdef OTA_KEY
                    "on"
#else
                    "off"
#endif
      );
    } else {
      handleMessage(in);
    }
    in = "";
  }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  M5.Display.setRotation(1);
  W = M5.Display.width();
  H = M5.Display.height();
  cv.createSprite(W, H);
  M5.Speaker.setVolume(120);

  prefs.begin("buddy");
  strlcpy(topic, prefs.getString("topic", DEFAULT_TOPIC).c_str(), sizeof(topic));
  topicParam.setValue(topic, 63);

  M5.update();
  setupWifi(M5.BtnB.isPressed());  // hold side button while powering on = WiFi setup
  Serial.printf("topic %s, portal %d\n", topic, portal);
}

void loop() {
  M5.update();
  if (M5.BtnA.wasPressed()) acknowledge();
  if (M5.BtnB.pressedFor(3000)) { WiFiManager().resetSettings(); message("WiFi cleared,", "rebooting to setup"); delay(1500); ESP.restart(); }
  pollSensors();
  pollSerial();
  if (portal) {
    pollPortal();
    if (state == IDLE && overlay == OV_NONE) { delay(30); return; }
    setupShown = false;
  } else {
    pollNtfy();
  }
  draw();
  delay(30);
}
