// Stack-chan (M5Stack CoreS3) 猫見守りカメラ
// 1時間に1回、または動体検知で撮影して Worker にアップロードする。
// 1時間ごとの撮影で猫が写っていれば、首を回して猫を写真の中央に合わせてもう1枚撮る。
// 夜間 (JST 0:00-6:00) は撮影も首振りもしない。
#include <M5StackChan.h>
#include <time.h>
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

// ---- 夜間停止 (JST) ----
// QUIET_START_HOUR 時から QUIET_END_HOUR 時までは撮影も首振りもしない (日付をまたいでもよい)。
// 設置場所を試すため、いまは 0 時開始。18 時に戻すときは Worker の送信 (18:05) と揃える
static const int QUIET_START_HOUR = 0;
static const int QUIET_END_HOUR = 6;

// ---- 猫を中央に合わせる (首のヨー軸) ----
// Worker が返す猫の枠の中心 cx (0=左端, 1=右端) から首の角度を決める。
// ホームは setCurrentPostionAsHome() でケージ正面に合わせておく
static const bool ENABLE_CENTERING = true;
static const float CAT_FOUND_SCORE = 0.6f;     // Worker の CAT_SCORE_THRESHOLD と揃える
static const int MOVE_SPEED = 300;             // 0-1000。ゆっくり動かして音と猫への刺激を抑える
static const float CAMERA_HFOV_DEG = 60.0f;    // カメラの水平画角 (仮の値。実機で合わせる)
static const int YAW_SIGN = 1;                 // ヨー角を増やすと画像の右側を向く (実機で確認済み)
static const float CENTER_DEADBAND = 0.08f;    // 中央からのずれがこれ未満なら動かさない (画像幅に対する割合)
static const int MAX_CENTER_YAW = 450;         // ホームから動かす上限 (0.1度単位)
static const uint32_t MOVE_TIMEOUT_MS = 8000;
static const uint32_t MOTION_SETTLE_MS = 2000;  // 首を動かした後、自動露出が落ち着くまで動体検知を止める

// 動体検知はフレームを GRID_W x GRID_H のセルに間引いて比較する
static const int GRID_W = 80;
static const int GRID_H = 60;

static uint8_t background[GRID_W * GRID_H];
static bool backgroundReady = false;
static uint8_t motionStreak = 0;
static uint32_t motionResumeMs = 0;

static GC0308 Camera;
static int currentYaw = 0;  // 0.1度単位。最後に猫を見つけた角度から次の周期を始める

static uint32_t lastHourlyMs = 0;
static uint32_t lastMotionShotMs = 0;
static uint16_t motionShotsToday = 0;
static uint32_t dayStartMs = 0;
static bool paused = false;  // 夜間停止中

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

// 時刻がまだ取れていなければ false (昼として動き続ける)
static bool isQuietHours() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;
  if (QUIET_START_HOUR <= QUIET_END_HOUR) return t.tm_hour >= QUIET_START_HOUR && t.tm_hour < QUIET_END_HOUR;
  return t.tm_hour >= QUIET_START_HOUR || t.tm_hour < QUIET_END_HOUR;  // 日付をまたぐ
}

// RGB565 は p[0]=RRRRRGGG, p[1]=GGGBBBBB の順（実機の生バイトで確認）。
// 2バイトの平均だと緑の下位ビットのノイズで大きく跳ねるため、緑 6bit を 0-255 に広げて輝度の代用にする
static inline uint8_t lumaAt(const camera_fb_t* fb, int x, int y) {
  const uint8_t* p = fb->buf + ((size_t)y * fb->width + x) * 2;
  uint8_t g = ((p[0] & 0x07) << 3) | (p[1] >> 5);
  return g << 2;
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
// cx には猫の枠の中心の横位置 (0-1) を入れる。猫がいない・位置が返らなければ -1
static float upload(const camera_fb_t* fb, const char* kind, float* cx) {
  *cx = -1.0f;
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
    String url = String(UPLOAD_URL) + "?kind=" + kind + "&w=" + fb->width;
    if (http.begin(client, url)) {
      http.setTimeout(20000);  // Worker 側の猫判定ぶん待つ
      http.addHeader("Authorization", String("Bearer ") + UPLOAD_TOKEN);
      http.addHeader("Content-Type", "image/jpeg");
      int code = http.POST(jpg, jpgLen);
      if (code >= 200 && code < 300) {
        // レスポンスは {"key":"...","cat":0.93,"cx":0.42}。猫がいなければ "cx":null
        String body = http.getString();
        int at = body.indexOf("\"cat\":");
        score = at >= 0 ? body.substring(at + 6).toFloat() : -1.0f;
        at = body.indexOf("\"cx\":");
        if (at >= 0 && !body.substring(at + 5).startsWith("null")) *cx = body.substring(at + 5).toFloat();
      }
      char msg[64];
      snprintf(msg, sizeof(msg), "%s: %d cat=%.2f cx=%.2f yaw=%d", kind, code, score, *cx, currentYaw);
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
static float captureAndUpload(const char* kind, float* cx) {
  *cx = -1.0f;
  for (int i = 0; i < 5; i++) {
    if (Camera.get()) Camera.free();
  }
  if (!Camera.get()) {
    showStatus("capture failed");
    return -2.0f;
  }
  float score = upload(Camera.fb, kind, cx);
  Camera.free();
  return score;
}

// 首を動かして、止まるまで待つ。動かした後は動体検知の背景を取り直す。
static void moveYawTo(int yaw) {
  M5StackChan.Motion.moveYaw(yaw, MOVE_SPEED);
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
  motionResumeMs = millis() + MOTION_SETTLE_MS;
}

// 猫が写真の中央からずれていれば、首を回して中央に合わせてもう1枚撮る。
static void centerOnCat(float cx) {
  if (cx < 0) return;  // Worker が位置を返さなかった
  float offset = cx - 0.5f;
  if (fabsf(offset) < CENTER_DEADBAND) return;
  int yaw = currentYaw + YAW_SIGN * (int)lroundf(offset * CAMERA_HFOV_DEG * 10.0f);
  if (yaw > MAX_CENTER_YAW) yaw = MAX_CENTER_YAW;
  if (yaw < -MAX_CENTER_YAW) yaw = -MAX_CENTER_YAW;
  if (yaw == currentYaw) return;
  moveYawTo(yaw);
  float centeredCx;
  captureAndUpload("center", &centeredCx);
}

static void hourlyRound() {
  float cx;
  float score = captureAndUpload("hourly", &cx);
  // 猫がいなくて首が横を向いていれば、ホームに戻って撮り直す (-1 判定失敗 / -2 通信失敗では動かさない)
  if (score >= 0 && score < CAT_FOUND_SCORE && currentYaw != 0) {
    moveYawTo(0);
    score = captureAndUpload("hourly", &cx);
  }
  if (ENABLE_CENTERING && score >= CAT_FOUND_SCORE) centerOnCat(cx);
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

  // 首は起動時の角度のままなので、ホームに戻して currentYaw = 0 と実際を揃える (ピッチは動かさない)
  moveYawTo(0);

  showStatus("connecting wifi...");
  WiFi.mode(WIFI_STA);
  bool wifiOk = ensureWifi();
  showStatus(wifiOk ? "wifi connected" : "wifi failed (will retry)");

  // 夜間停止の判定に JST の時刻を使う。Wi-Fi が後から繋がれば SNTP がその時に合わせる
  configTzTime("JST-9", "ntp.nict.jp", "pool.ntp.org");
  struct tm t;
  if (wifiOk && getLocalTime(&t, 10000)) {
    char msg[32];
    strftime(msg, sizeof(msg), "time %H:%M JST", &t);
    showStatus(msg);
  } else {
    showStatus("time sync failed (run as daytime)");
  }

  lastHourlyMs = millis();
  dayStartMs = millis();
  paused = isQuietHours();
  if (paused) {
    showStatus("quiet hours: paused");
  } else {
    hourlyRound();  // 起動確認を兼ねて1回
  }
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

  // 夜間は撮影も首振りもしない。朝になったら1回撮ってから再開する
  bool quiet = isQuietHours();
  if (quiet != paused) {
    paused = quiet;
    showStatus(paused ? "quiet hours: paused" : "quiet hours: resumed");
    if (paused && currentYaw != 0) moveYawTo(0);  // 夜の間はホームを向いて止まる
    if (!paused) {
      motionShotsToday = 0;
      backgroundReady = false;
      lastHourlyMs = now;
      hourlyRound();
      return;
    }
  }
  if (paused) {
    delay(1000);
    return;
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
  // 首を動かした直後は露出が変わっていく途中なので、背景を取らずに待つ
  if ((int32_t)(now - motionResumeMs) < 0) {
    Camera.free();
    delay(50);
    return;
  }
  bool motion = detectMotion(Camera.fb);
  Camera.free();

  if (motion && now - lastMotionShotMs >= MOTION_COOLDOWN_MS &&
      motionShotsToday < MAX_MOTION_SHOTS_PER_DAY) {
    lastMotionShotMs = now;
    motionShotsToday++;
    motionStreak = 0;
    float cx;
    captureAndUpload("motion", &cx);  // 動体検知では首を動かさない (猫を驚かせないように)
    backgroundReady = false;  // 撮影中の時間経過ぶん背景を取り直す
  }
  delay(50);
}
