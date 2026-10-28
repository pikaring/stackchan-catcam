// Stack-chan (M5Stack CoreS3) 猫見守りカメラ
// 1時間に1回、または動体検知で撮影して Worker にアップロードする。
// 1時間ごとの撮影では猫を探す: 写っていなければ首の向きを変えて撮り、
// 猫が少しでも写っていればその方向に首を回して、猫を写真の中央に合わせてもう1枚撮る。
// 夜間 (JST 0:00-6:00) と、起動後に時刻が取れるまでは撮影も首振りもしない。
#include <M5StackChan.h>
#include <time.h>
#include <esp_system.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "img_converters.h"
#include "gc0308.h"
#include "clawd.h"
#include "secrets.h"

// ---- 調整用パラメータ ----
static const uint32_t HOURLY_INTERVAL_MS = 60UL * 60UL * 1000UL;
static const uint32_t MOTION_COOLDOWN_MS = 60UL * 1000UL;  // 動体検知の連続撮影を抑える
static const uint16_t MAX_MOTION_SHOTS_PER_DAY = 150;      // 猫が動き続けても上限で止める
static const uint8_t JPEG_QUALITY = 12;                    // 小さいほど高画質 (10-63)
static const uint8_t CELL_DIFF_THRESHOLD = 24;             // 1セルの輝度差がこれ以上で「変化」
static const float MOTION_RATIO = 0.04f;                   // 変化セルがこの割合以上で動体とみなす
static const uint8_t MOTION_CONFIRM_FRAMES = 2;            // 連続して検知したら確定 (ノイズ対策)
static const uint32_t DISPLAY_DIM_MS = 15UL * 1000UL;      // 起動後、画面を暗くするまでの時間
static const uint8_t DISPLAY_DIM_BRIGHTNESS = 24;           // 暗くした後の明るさ (0-255)。動いていると分かる程度で、猫が気にしないよう暗め

// ---- 夜間停止 (JST) ----
// QUIET_START_HOUR 時から QUIET_END_HOUR 時までは撮影も首振りもしない (日付をまたいでもよい)。
// 設置場所を試すため、いまは 0 時開始。18 時に戻すときは Worker の送信 (18:05) と揃える
static const int QUIET_START_HOUR = 0;
static const int QUIET_END_HOUR = 6;

// ---- 猫を探して中央に合わせる (首のヨー軸とピッチ軸) ----
// Worker が返す猫の枠の中心 cx (0=左端, 1=右端)、cy (0=上端, 1=下端) から首の角度を決める。
// ホームは setCurrentPostionAsHome() で部屋の見たい方向に合わせておく。
// ピッチはホームが下限 (0) で、角度を増やすと上を向く (ライブラリの可動範囲は 0-90°、公式推奨は 5-85°)。
// ホームは画面を台に垂直にした姿勢で設定する。保存されたゼロが物理的な下限より下にずれていると
// 0 や小さい角度を指示しても首が下限に当たって動かない (2026-10-27 に設定し直して解消)
static const bool ENABLE_CENTERING = true;
static const float CAT_HINT_SCORE = 0.3f;      // これ以上なら「猫が少し写っている」とみなして向ける (送信対象は Worker の 0.6 以上)
// 猫がいないときに見る向き {ヨー, ピッチ} (0.1度単位、この順に回る)。
// 正面 → 右 → 斜め右上 → 上 → 斜め左上 → 左。猫は部屋の右上にいることが多い
struct HeadPos {
  int yaw;
  int pitch;
};
static const HeadPos SEARCH_POSITIONS[] = {{0, 0}, {450, 0}, {450, 250}, {0, 250}, {-450, 250}, {-450, 0}};
static const int MAX_AIM_STEPS = 2;            // 中央に合わせ直す回数の上限 (端に少し写った猫は1回では中央に来ない)
static const int MOVE_SPEED = 300;             // 0-1000。ゆっくり動かして音と猫への刺激を抑える
static const float CAMERA_HFOV_DEG = 60.0f;    // カメラの水平画角 (仮の値。実機で合わせる)
static const float CAMERA_VFOV_DEG = 45.0f;    // カメラの垂直画角 (水平 60° と 4:3 からの計算値)
static const int YAW_SIGN = 1;                 // ヨー角を増やすと画像の右側を向く (実機で確認済み)
static const int PITCH_SIGN = 1;               // ピッチ角を増やすと画像の上側を向く (実機で確認済み)
static const float CENTER_DEADBAND = 0.08f;    // 中央からのずれがこれ未満なら動かさない (画像幅に対する割合)
static const int MAX_CENTER_YAW = 600;         // ホームから動かす上限 (0.1度単位)
static const int MAX_PITCH = 400;              // ピッチを上げる上限 (0.1度単位)。下はホーム (0) まで
static const uint32_t MOVE_TIMEOUT_MS = 8000;
static const uint32_t MOTION_SETTLE_MS = 2000;  // 首を動かした後、自動露出が落ち着くまで動体検知を止める

// 部屋が暗いときは撮らない (真っ暗な写真と、暗い所のノイズによる動体の誤検知を避ける)。
// 画面の平均輝度 (緑成分、0-255) が DARK_LUMA_ON 未満で暗いとみなし、DARK_LUMA_OFF 以上に戻るまで暗いままにする。
// 1つの基準だと、その近くの明るさの部屋で暗い/明るいが数秒おきに入れ替わり、そのたびに動体検知の背景を取り直していた。
// 実測 (写真の輝度): 夜の照明下で 45-80、消灯後や朝6時の暗い部屋で 13-29
static const uint8_t DARK_LUMA_ON = 30;
static const uint8_t DARK_LUMA_OFF = 40;

// 動体検知はフレームを GRID_W x GRID_H のセルに間引いて比較する
static const int GRID_W = 80;
static const int GRID_H = 60;

static uint8_t background[GRID_W * GRID_H];
static bool backgroundReady = false;
static bool dark = false;  // 最後に見たフレームが暗かった
static uint8_t motionStreak = 0;
static uint32_t motionResumeMs = 0;

static GC0308 Camera;
static clawd::Animator Clawd;  // 画面のマスコット。動いていることを示す
static int currentYaw = 0;  // 0.1度単位。最後に猫を見つけた角度から次の周期を始める
static int currentPitch = 0;

static uint32_t lastHourlyMs = 0;
static uint32_t lastMotionShotMs = 0;
static uint16_t motionShotsToday = 0;
static uint32_t dayStartMs = 0;
static bool paused = false;  // 夜間停止中、または時刻がまだ取れていない
static uint32_t lastWifiRetryMs = 0;
static const uint32_t WIFI_RETRY_MS = 30UL * 1000UL;  // 時刻待ちの間に Wi-Fi を繋ぎ直す間隔

// 再起動の原因を調べるため、起動後最初のアップロードに付けて R2 のメタデータに残す。
// bootCount は電源を入れてからの起動回数 (ソフトリセットやクラッシュでは消えない RTC メモリ)
RTC_NOINIT_ATTR static uint32_t bootMagic;
RTC_NOINIT_ATTR static uint32_t bootCount;
static String bootInfo;  // 送ったら空にする

static const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "unknown";
  }
}

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

// 時刻がまだ取れていなければ true (夜中の再起動で撮らないよう、時刻が分かるまで止まる)
// 今の時刻 (JST) を読む。まだ NTP で合わせていなければ false。
// getLocalTime(&t, 0) は呼んだ瞬間に millis() が進むと一度も時刻を読まずに false を返すので使わない
// (それで夜間停止→再開が誤って起き、1時間ごとの撮影が早まっていた)
static bool readLocalTime(struct tm* t) {
  time_t now = time(nullptr);
  localtime_r(&now, t);
  return t->tm_year > (2016 - 1900);
}

static bool isQuietHours() {
  struct tm t;
  if (!readLocalTime(&t)) return true;
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
// 日差しや自動露出で画面全体の明るさが一度に変わると全セルに差が出るので、
// 差の中央値 (画面全体の明るさの変化) を引いてから変化セルを数える。
static bool detectMotion(const camera_fb_t* fb) {
  static int16_t diffs[GRID_W * GRID_H];
  static uint16_t hist[511];  // 差 -255..255 のヒストグラム (中央値用)
  const int stepX = fb->width / GRID_W;
  const int stepY = fb->height / GRID_H;
  const int cells = GRID_W * GRID_H;
  memset(hist, 0, sizeof(hist));
  for (int gy = 0; gy < GRID_H; gy++) {
    for (int gx = 0; gx < GRID_W; gx++) {
      int i = gy * GRID_W + gx;
      uint8_t cur = lumaAt(fb, gx * stepX, gy * stepY);
      uint8_t& bg = background[i];
      if (!backgroundReady) {
        bg = cur;
        continue;
      }
      int diff = (int)cur - (int)bg;
      diffs[i] = diff;
      hist[diff + 255]++;
      bg = (uint8_t)((int)bg + (diff >> 2));
    }
  }
  if (!backgroundReady) {
    backgroundReady = true;
    return false;
  }
  int median = 0;
  for (int b = 0, seen = 0; b < 511; b++) {
    seen += hist[b];
    if (seen * 2 >= cells) {
      median = b - 255;
      break;
    }
  }
  int changed = 0;
  for (int i = 0; i < cells; i++) {
    int d = diffs[i] - median;
    if (d < 0) d = -d;
    if (d >= CELL_DIFF_THRESHOLD) changed++;
  }
  bool over = changed >= (int)(cells * MOTION_RATIO);
  motionStreak = over ? motionStreak + 1 : 0;
  return motionStreak >= MOTION_CONFIRM_FRAMES;
}

// フレームの平均輝度 (動体検知と同じセルで間引いて求める)
static uint8_t frameLuma(const camera_fb_t* fb) {
  const int stepX = fb->width / GRID_W;
  const int stepY = fb->height / GRID_H;
  uint32_t sum = 0;
  for (int gy = 0; gy < GRID_H; gy++) {
    for (int gx = 0; gx < GRID_W; gx++) sum += lumaAt(fb, gx * stepX, gy * stepY);
  }
  return sum / (GRID_W * GRID_H);
}

// 明るさが変わったときだけ表示する
// フレームの明るさから暗いかどうかを決める (幅を持たせて切り替える)
static void updateDark(const camera_fb_t* fb) {
  uint8_t luma = frameLuma(fb);
  bool now = dark ? luma < DARK_LUMA_OFF : luma < DARK_LUMA_ON;
  if (now == dark) return;
  dark = now;
  char msg[48];
  snprintf(msg, sizeof(msg), dark ? "too dark (luma=%u): not shooting" : "light (luma=%u): shooting resumed", luma);
  showStatus(msg);
}

// アップロードして、首の向きを決めるための猫スコア (Worker の aim) を返す。
// aim は物体検出のスコア。物体検出が見落としたときは画像を読む LLM の判定で補った値になる。
// 0以上: 猫スコア / -1: Worker の猫判定が失敗 / -2: アップロード失敗
// cx, cy には猫の中心の位置 (0-1) を入れる。猫がいない・位置が返らなければ -1
static float upload(const camera_fb_t* fb, const char* kind, float* cx, float* cy) {
  *cx = -1.0f;
  *cy = -1.0f;
  uint8_t* jpg = nullptr;
  size_t jpgLen = 0;
  if (!frame2jpg(const_cast<camera_fb_t*>(fb), 100 - JPEG_QUALITY, &jpg, &jpgLen)) {
    showStatus("jpeg encode failed");
    return -2.0f;
  }
  float score = -2.0f;
  float catScore = -2.0f;
  if (ensureWifi()) {
    WiFiClientSecure client;
    client.setInsecure();  // 証明書検証なし。気になる場合は setCACert で固定する
    HTTPClient http;
    String url = String(UPLOAD_URL) + "?kind=" + kind + "&w=" + fb->width + "&h=" + fb->height + bootInfo;
    if (http.begin(client, url)) {
      http.setTimeout(30000);  // Worker 側の猫判定 (物体検出 + LLM) ぶん待つ
      http.addHeader("Authorization", String("Bearer ") + UPLOAD_TOKEN);
      http.addHeader("Content-Type", "image/jpeg");
      int code = http.POST(jpg, jpgLen);
      if (code >= 200 && code < 300) {
        // レスポンスは {"key":"...","cat":0.93,"cx":0.42,"cy":0.3,"aim":0.93,"aimCx":0.42,"aimCy":0.3}。位置がなければ null
        String body = http.getString();
        int at = body.indexOf("\"cat\":");
        catScore = at >= 0 ? body.substring(at + 6).toFloat() : -1.0f;
        at = body.indexOf("\"aim\":");
        score = at >= 0 ? body.substring(at + 6).toFloat() : catScore;
        at = body.indexOf("\"aimCx\":");
        if (at >= 0 && !body.substring(at + 8).startsWith("null")) *cx = body.substring(at + 8).toFloat();
        at = body.indexOf("\"aimCy\":");
        if (at >= 0 && !body.substring(at + 8).startsWith("null")) *cy = body.substring(at + 8).toFloat();
        bootInfo = "";
      }
      char msg[96];
      snprintf(msg, sizeof(msg), "%s: %d cat=%.2f aim=%.2f cx=%.2f cy=%.2f yaw=%d pitch=%d", kind, code, catScore, score,
               *cx, *cy, currentYaw, currentPitch);
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
// 返り値は upload() と同じ。暗くて撮らなかったときは -3
static float captureAndUpload(const char* kind, float* cx, float* cy) {
  *cx = -1.0f;
  *cy = -1.0f;
  for (int i = 0; i < 5; i++) {
    if (Camera.get()) Camera.free();
  }
  if (!Camera.get()) {
    showStatus("capture failed");
    return -2.0f;
  }
  updateDark(Camera.fb);
  if (dark) {
    Camera.free();
    return -3.0f;
  }
  float score = upload(Camera.fb, kind, cx, cy);
  Camera.free();
  return score;
}

// 動き出した首が止まるまで待つ
static void waitForHead() {
  uint32_t start = millis();
  delay(100);
  while (M5StackChan.Motion.isMoving() && millis() - start < MOVE_TIMEOUT_MS) {
    M5StackChan.update();
    delay(50);
  }
  if (M5StackChan.Motion.isMoving()) M5StackChan.Motion.stop();
  delay(500);  // 揺れが収まるのを待つ
}

// 首を動かして、止まるまで待つ。サーボに一度にかかる負担を抑えるため、ヨーとピッチは片方ずつ動かす。
// force なら今の角度と同じでも動かす (起動直後は実際の角度が分からないため)。
// 動かした後は動体検知の背景を取り直す。
static void moveHeadTo(int yaw, int pitch, bool force = false) {
  if (force || yaw != currentYaw) {
    M5StackChan.Motion.moveYaw(yaw, MOVE_SPEED);
    waitForHead();
  }
  if (force || pitch != currentPitch) {
    M5StackChan.Motion.movePitch(pitch, MOVE_SPEED);
    waitForHead();
  }
  currentYaw = yaw;
  currentPitch = pitch;
  backgroundReady = false;
  motionStreak = 0;
  motionResumeMs = millis() + MOTION_SETTLE_MS;
}

static bool atHome() { return currentYaw == 0 && currentPitch == 0; }

// 猫が写真の中央からずれていれば、首を左右・上下に動かして中央に合わせてもう1枚撮る。
// 合わせた後も猫が写っていてまだずれていれば、MAX_AIM_STEPS 回まで繰り返す。
// 縦の位置が返らなければ (cy < 0) 左右だけ合わせる。
// 最後に撮った写真の猫スコアを返す (猫を見失ったら CAT_HINT_SCORE 未満)
static float centerOnCat(float score, float cx, float cy) {
  for (int step = 0; step < MAX_AIM_STEPS; step++) {
    if (score < CAT_HINT_SCORE || cx < 0) return score;  // 猫を見失った / Worker が位置を返さなかった
    float offsetX = cx - 0.5f;
    float offsetY = cy >= 0 ? 0.5f - cy : 0.0f;  // 上にずれていれば正
    int yaw = currentYaw;
    int pitch = currentPitch;
    if (fabsf(offsetX) >= CENTER_DEADBAND) yaw += YAW_SIGN * (int)lroundf(offsetX * CAMERA_HFOV_DEG * 10.0f);
    if (fabsf(offsetY) >= CENTER_DEADBAND) pitch += PITCH_SIGN * (int)lroundf(offsetY * CAMERA_VFOV_DEG * 10.0f);
    yaw = constrain(yaw, -MAX_CENTER_YAW, MAX_CENTER_YAW);
    pitch = constrain(pitch, 0, MAX_PITCH);
    if (yaw == currentYaw && pitch == currentPitch) return score;
    moveHeadTo(yaw, pitch);
    score = captureAndUpload("center", &cx, &cy);
  }
  return score;
}

// 今の向きで撮り、猫が少しでも写っていれば中央に合わせる。
// 写っていなければ SEARCH_POSITIONS の向きを順に見て回り、見つけた所で中央に合わせる。どこにもいなければホームに戻る。
// 中央に合わせる途中で猫を見失ったときもホームに戻る。
static void hourlyRound() {
  float cx, cy;
  float score = captureAndUpload("hourly", &cx, &cy);
  if (score < 0) return;  // -1 判定失敗 / -2 通信失敗 / -3 暗いときは首を動かさない
  if (ENABLE_CENTERING && score >= CAT_HINT_SCORE) {
    if (centerOnCat(score, cx, cy) < CAT_HINT_SCORE && !atHome()) moveHeadTo(0, 0);
    return;
  }
  if (ENABLE_CENTERING) {
    for (const HeadPos& pos : SEARCH_POSITIONS) {
      if (pos.yaw == currentYaw && pos.pitch == currentPitch) continue;  // この向きはもう撮った
      moveHeadTo(pos.yaw, pos.pitch);
      score = captureAndUpload("search", &cx, &cy);
      if (score < 0) break;
      if (score >= CAT_HINT_SCORE) {
        if (centerOnCat(score, cx, cy) >= CAT_HINT_SCORE) return;
        break;  // 見失ったらホームに戻る
      }
    }
  }
  if (!atHome()) moveHeadTo(0, 0);  // 猫がいなければホームを向いて動体検知に戻る
}

void setup() {
  M5StackChan.begin();  // 内部で M5.begin()、サーボ電源、サーボ通信を初期化する
  Serial.begin(115200);
  M5StackChan.Display().setTextSize(1);
  M5StackChan.Display().setTextColor(TFT_WHITE, TFT_BLACK);
  Clawd.begin(&M5StackChan.Display());

  if (bootMagic != 0xCA7CA7) {
    bootMagic = 0xCA7CA7;
    bootCount = 0;
  }
  bootCount++;
  esp_reset_reason_t reason = esp_reset_reason();
  bootInfo = String("&reset=") + resetReasonName(reason) + "&boots=" + bootCount + "&heap=" + ESP.getFreeHeap();
  char bootMsg[48];
  snprintf(bootMsg, sizeof(bootMsg), "boot #%lu reset=%s", (unsigned long)bootCount, resetReasonName(reason));
  showStatus(bootMsg);

  if (!Camera.begin()) {
    showStatus("camera init failed");
    while (true) delay(1000);
  }

  // 首は起動時の角度のままなので、ホームに戻して currentYaw/currentPitch = 0 と実際を揃える
  moveHeadTo(0, 0, true);

  showStatus("connecting wifi...");
  WiFi.mode(WIFI_STA);
  bool wifiOk = ensureWifi();
  showStatus(wifiOk ? "wifi connected" : "wifi failed (will retry)");
  bootInfo += wifiOk ? "&wifi=ok" : "&wifi=fail";

  // 夜間停止の判定に JST の時刻を使う。Wi-Fi が後から繋がれば SNTP がその時に合わせる
  configTzTime("JST-9", "ntp.nict.jp", "pool.ntp.org");
  struct tm t;
  if (wifiOk && getLocalTime(&t, 10000)) {
    char msg[32];
    strftime(msg, sizeof(msg), "time %H:%M JST", &t);
    showStatus(msg);
  } else {
    showStatus("time sync failed (wait before shooting)");
  }

  lastHourlyMs = millis();
  dayStartMs = millis();
  paused = isQuietHours();
  lastWifiRetryMs = millis();
  if (paused) {
    showStatus("quiet hours: paused");
  } else {
    hourlyRound();  // 起動確認を兼ねて1回
  }
}

void loop() {
  M5StackChan.update();
  uint32_t now = millis();

  // 起動後しばらくしたら画面を暗くする (猫が気にしないように・省電力)。
  // 消すと動いているか分からないので、暗いまま Clawd を動かし続ける (夜間や暗いときは眠る)
  if (now > DISPLAY_DIM_MS) M5StackChan.Display().setBrightness(DISPLAY_DIM_BRIGHTNESS);
  Clawd.update(now, paused || dark);

  if (now - dayStartMs >= 24UL * 60UL * 60UL * 1000UL) {
    dayStartMs = now;
    motionShotsToday = 0;
  }

  // 夜間は撮影も首振りもしない。朝になったら1回撮ってから再開する
  bool quiet = isQuietHours();
  if (quiet != paused) {
    paused = quiet;
    showStatus(paused ? "quiet hours: paused" : "quiet hours: resumed");
    if (paused && !atHome()) moveHeadTo(0, 0);  // 夜の間はホームを向いて止まる
    if (!paused) {
      motionShotsToday = 0;
      backgroundReady = false;
      lastHourlyMs = now;
      hourlyRound();
      return;
    }
  }
  if (paused) {
    // 時刻が取れていなければ Wi-Fi を繋ぎ直す (繋がれば SNTP が時刻を合わせる)
    struct tm t;
    if (!readLocalTime(&t) && now - lastWifiRetryMs >= WIFI_RETRY_MS) {
      lastWifiRetryMs = now;
      ensureWifi();
    }
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
  // 暗いときは動体検知もしない。明るくなったら背景を取り直す
  updateDark(Camera.fb);
  if (dark) {
    Camera.free();
    backgroundReady = false;
    motionStreak = 0;
    delay(500);
    return;
  }
  bool motion = detectMotion(Camera.fb);
  Camera.free();

  if (motion && now - lastMotionShotMs >= MOTION_COOLDOWN_MS &&
      motionShotsToday < MAX_MOTION_SHOTS_PER_DAY) {
    lastMotionShotMs = now;
    motionShotsToday++;
    motionStreak = 0;
    float cx, cy;
    captureAndUpload("motion", &cx, &cy);  // 動体検知では首を動かさない (猫を驚かせないように)
    backgroundReady = false;  // 撮影中の時間経過ぶん背景を取り直す
  }
  delay(50);
}
