// Stack-chan (M5Stack CoreS3) 猫見守りカメラ
// 1時間に1回、または動体検知で撮影して Worker にアップロードする。
// 1時間ごとの撮影で猫が写っていなければ、首を振って探す。
#include <M5StackChan.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "img_converters.h"
#include "gc0308.h"
#include "secrets.h"

// ---- 調整用パラメータ ----
static const uint32_t HOURLY_INTERVAL_MS = 60UL * 60UL * 1000UL;
static const uint32_t MOTION_COOLDOWN_MS = 60UL * 1000UL;  // 動体検知の連続撮影を抑える
static const uint16_t MAX_MOTION_SHOTS_PER_DAY = 150;      // 猫が動き続けても上限で止める
static const uint8_t JPEG_QUALITY = 12;                    // 小さいほど高画質 (10-63)
static const uint8_t CELL_DIFF_THRESHOLD = 24;             // 1セルの輝度差がこれ以上で「変化」
static const float MOTION_RATIO = 0.04f;                   // 変化セルがこの割合以上で動体とみなす
static const uint8_t MOTION_CONFIRM_FRAMES = 2;            // 連続して検知したら確定 (ノイズ対策)
static const uint32_t DISPLAY_SLEEP_MS = 15UL * 1000UL;    // 起動後、画面を消すまでの時間

// ---- 首振り探索 ----
static const bool ENABLE_SEARCH = true;
static const float CAT_FOUND_SCORE = 0.6f;     // Worker の CAT_SCORE_THRESHOLD と揃える
static const int SEARCH_SPEED = 300;           // 0-1000。ゆっくり動かして音と猫への刺激を抑える
// ホーム位置からのヨー角 (0.1度単位)。ホームは setCurrentPostionAsHome() でケージ正面に合わせておく
static const int SEARCH_YAW[] = {-300, 300, -600, 600};
static const uint32_t MOVE_TIMEOUT_MS = 8000;

// 動体検知はフレームを GRID_W x GRID_H のセルに間引いて比較する
static const int GRID_W = 80;
static const int GRID_H = 60;

static uint8_t background[GRID_W * GRID_H];
static bool backgroundReady = false;
static uint8_t motionStreak = 0;

static GC0308 Camera;
static int currentYaw = 0;  // 0.1度単位。最後に猫を見つけた角度から次の周期を始める

static uint32_t lastHourlyMs = 0;
static uint32_t lastMotionShotMs = 0;
static uint16_t motionShotsToday = 0;
static uint32_t dayStartMs = 0;

static void showStatus(const char* msg) {
  M5StackChan.Display().fillRect(0, 0, 320, 24, TFT_BLACK);
  M5StackChan.Display().setCursor(4, 4);
  M5StackChan.Display().print(msg);
  Serial.println(msg);
}

static bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

// RGB565 のバイト順に依存しないよう、2バイトの平均を輝度の代用にする
static inline uint8_t lumaAt(const camera_fb_t* fb, int x, int y) {
  const uint8_t* p = fb->buf + ((size_t)y * fb->width + x) * 2;
  return (uint8_t)(((uint16_t)p[0] + p[1]) >> 1);
}

// 背景(移動平均)との差分で動体を判定する。ゆっくり動く猫も拾える。
static bool detectMotion(const camera_fb_t* fb) {
  const int stepX = fb->width / GRID_W;
  const int stepY = fb->height / GRID_H;
  int changed = 0;
  for (int gy = 0; gy < GRID_H; gy++) {
    for (int gx = 0; gx < GRID_W; gx++) {
      uint8_t cur = lumaAt(fb, gx * stepX, gy * stepY);
      uint8_t& bg = background[gy * GRID_W + gx];
      if (!backgroundReady) {
        bg = cur;
        continue;
      }
      int diff = (int)cur - (int)bg;
      if (diff < 0) diff = -diff;
      if (diff >= CELL_DIFF_THRESHOLD) changed++;
      bg = (uint8_t)((int)bg + (((int)cur - (int)bg) >> 2));
    }
  }
  if (!backgroundReady) {
    backgroundReady = true;
    return false;
  }
  bool over = changed >= (int)(GRID_W * GRID_H * MOTION_RATIO);
  motionStreak = over ? motionStreak + 1 : 0;
  return motionStreak >= MOTION_CONFIRM_FRAMES;
}

// アップロードして Worker が返す猫スコアを返す。
// 0以上: 猫スコア / -1: Worker の猫判定が失敗 / -2: アップロード失敗
static float upload(const camera_fb_t* fb, const char* kind) {
  uint8_t* jpg = nullptr;
  size_t jpgLen = 0;
  if (!frame2jpg(const_cast<camera_fb_t*>(fb), 100 - JPEG_QUALITY, &jpg, &jpgLen)) {
    showStatus("jpeg encode failed");
    return -2.0f;
  }
  float score = -2.0f;
  if (ensureWifi()) {
    WiFiClientSecure client;
    client.setInsecure();  // 証明書検証なし。気になる場合は setCACert で固定する
    HTTPClient http;
    String url = String(UPLOAD_URL) + "?kind=" + kind;
    if (http.begin(client, url)) {
      http.setTimeout(20000);  // Worker 側の猫判定ぶん待つ
      http.addHeader("Authorization", String("Bearer ") + UPLOAD_TOKEN);
      http.addHeader("Content-Type", "image/jpeg");
      int code = http.POST(jpg, jpgLen);
      if (code >= 200 && code < 300) {
        // レスポンスは {"key":"...","cat":0.93}
        String body = http.getString();
        int at = body.indexOf("\"cat\":");
        score = at >= 0 ? body.substring(at + 6).toFloat() : -1.0f;
      }
      char msg[56];
      snprintf(msg, sizeof(msg), "%s: %d cat=%.2f", kind, code, score);
      showStatus(msg);
      http.end();
    }
  } else {
    showStatus("wifi unavailable");
  }
  free(jpg);
  return score;
}

// 撮影してアップロード。AE が落ち着くよう数フレーム捨ててから撮る。
static float captureAndUpload(const char* kind) {
  for (int i = 0; i < 5; i++) {
    if (Camera.get()) Camera.free();
  }
  if (!Camera.get()) {
    showStatus("capture failed");
    return -2.0f;
  }
  float score = upload(Camera.fb, kind);
  Camera.free();
  return score;
}

// 首を動かして、止まるまで待つ。動かした後は動体検知の背景を取り直す。
static void moveYawTo(int yaw) {
  M5StackChan.Motion.moveYaw(yaw, SEARCH_SPEED);
  uint32_t start = millis();
  delay(100);
  while (M5StackChan.Motion.isMoving() && millis() - start < MOVE_TIMEOUT_MS) {
    M5StackChan.update();
    delay(50);
  }
  if (M5StackChan.Motion.isMoving()) M5StackChan.Motion.stop();
  delay(500);  // 揺れが収まるのを待つ
  currentYaw = yaw;
  backgroundReady = false;
  motionStreak = 0;
}

// 現在の向きで猫が写らなかったとき、首を振って探す。
// 猫が見つかった位置で止まる。見つからなければ最もスコアの高かった向きに戻る。
static void searchForCat(float currentScore) {
  float bestScore = currentScore;
  int bestYaw = currentYaw;
  for (int yaw : SEARCH_YAW) {
    if (yaw == currentYaw) continue;
    moveYawTo(yaw);
    float score = captureAndUpload("search");
    if (score > bestScore) {
      bestScore = score;
      bestYaw = yaw;
    }
    if (score >= CAT_FOUND_SCORE) break;
  }
  if (bestYaw != currentYaw) moveYawTo(bestYaw);
}

static void hourlyRound() {
  float score = captureAndUpload("hourly");
  // -1 (判定失敗) と -2 (通信失敗) のときは探さない
  if (ENABLE_SEARCH && score >= 0 && score < CAT_FOUND_SCORE) searchForCat(score);
}

void setup() {
  M5StackChan.begin();  // 内部で M5.begin()、サーボ電源、サーボ通信を初期化する
  Serial.begin(115200);
  M5StackChan.Display().setTextSize(1);
  M5StackChan.Display().setTextColor(TFT_WHITE, TFT_BLACK);

  if (!Camera.begin()) {
    showStatus("camera init failed");
    while (true) delay(1000);
  }

  showStatus("connecting wifi...");
  WiFi.mode(WIFI_STA);
  showStatus(ensureWifi() ? "wifi connected" : "wifi failed (will retry)");

  lastHourlyMs = millis();
  dayStartMs = millis();
  hourlyRound();  // 起動確認を兼ねて1回
}

void loop() {
  M5StackChan.update();
  uint32_t now = millis();

  // 起動後しばらくしたら画面を消す (猫が気にしないように・省電力)
  if (now > DISPLAY_SLEEP_MS) M5StackChan.Display().setBrightness(0);

  if (now - dayStartMs >= 24UL * 60UL * 60UL * 1000UL) {
    dayStartMs = now;
    motionShotsToday = 0;
  }

  if (now - lastHourlyMs >= HOURLY_INTERVAL_MS) {
    lastHourlyMs = now;
    hourlyRound();
    return;
  }

  if (!Camera.get()) {
    delay(20);
    return;
  }
  bool motion = detectMotion(Camera.fb);
  Camera.free();

  if (motion && now - lastMotionShotMs >= MOTION_COOLDOWN_MS &&
      motionShotsToday < MAX_MOTION_SHOTS_PER_DAY) {
    lastMotionShotMs = now;
    motionShotsToday++;
    motionStreak = 0;
    captureAndUpload("motion");
    backgroundReady = false;  // 撮影中の時間経過ぶん背景を取り直す
  }
  delay(50);
}
