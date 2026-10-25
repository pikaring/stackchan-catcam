# Stack-chan 猫見守りカメラ

Stack-chan（M5Stack CoreS3）で猫を見守り、写真をクラウドに上げ、毎日4枚を母親にLINEで送る。

```
Stack-chan --HTTPS POST--> Cloudflare Worker --> R2 (写真保存)
  ・1時間ごと                    |
  ・動体検知                     +-- 毎日18:05 JST: 当日の4枚を選んでLINE Flexメッセージ1通で送信
  ・0:00-6:00 JST は停止
```

- 動体検知はカメラ映像（VGA）を80x60セルに間引き、移動平均の背景との差分で判定する。
  60秒のクールダウンと1日150枚の上限あり。
- 夜間停止: JST 0:00〜6:00 は撮影も首振りもしない（時刻は起動時に NTP で合わせる）。6:00 に1枚撮って再開する。
  設置場所を試している間は 0 時開始にしている。予定では 18:00 開始にして、送信（18:05）の直前に止める。
  18:05 以降に撮った写真は、その日の送信にも翌日の送信にも入らない。
  時刻が取れないときは昼として動き続ける。
- 猫を中央に合わせる: 1時間ごとの撮影で猫が写っていれば、Worker が返す猫の枠の中心（`cx`、0=左端〜1=右端）から首（ヨー軸）を回し、
  猫を写真の中央に合わせてもう1枚撮る（`kind=center`、ホームから ±45° まで）。猫がいなくて首が横を向いていればホームに戻って撮り直す。
  起動時と夜間停止に入るときもホームに戻す。動体検知では首を動かさない。首が動いた後は動体検知の背景を取り直す。
- 猫判定: アップロード時に Workers AI の物体検出（`@cf/facebook/detr-resnet-50`）で "cat" のスコアを求め、R2のメタデータ（`cat`）に保存する。判定失敗は `-1`（不明）で、送信対象にならない。
- 4枚の選び方: スコア0.6以上の写真だけを対象に、時系列で4等分した各区間から猫スコア最大の1枚ずつ。対象が4枚以下ならそのまま全部。
  猫が1枚も写っていない日は「猫が写った写真がありませんでした」というテキストを送る。
- LINEはメッセージ1通（2x2のFlex）にまとめている。メッセージ数の無料枠に収めるため。

## 1. Worker (クラウド側)

```sh
cd worker
npx wrangler r2 bucket create stackchan-catcam
npx wrangler secret put UPLOAD_TOKEN              # 適当な長いランダム文字列
npx wrangler secret put SIGNING_SECRET            # 同上（画像URLの署名用）
npx wrangler secret put LINE_CHANNEL_ACCESS_TOKEN # 下記のLINE設定で取得
npx wrangler deploy
```

猫判定に Workers AI（`wrangler.toml` の `[ai]` バインディング）を使う。無料枠の範囲は Cloudflare の料金ページで確認すること。

デプロイ後に出る URL を `wrangler.toml` の `PUBLIC_BASE_URL` に書いて、もう一度 `wrangler deploy`。

R2 のライフサイクルルールで `photos/` を30日で削除しておく（Cloudflare ダッシュボード → R2 → バケット → 設定）。
画像URLの有効期限も30日。

## 2. LINE

LINE Notify は終了しているので Messaging API を使う。

1. LINE Developers で Messaging API チャネルを作り、チャネルアクセストークン（長期）を発行する。
2. 母親と自分で、そのLINE公式アカウントを友だち追加する。
3. 既定は友だち全員への broadcast。特定の人だけに送りたい場合は `LINE_TO` に userId を入れると push になる。

動作確認（18:05 を待たずに送る）:

```sh
curl -X POST -H "Authorization: Bearer $UPLOAD_TOKEN" https://<worker-url>/digest
```

## 3. ファームウェア

対象は Kickstarter 版 StackChan（CoreS3 + SCS0009 サーボ）。公式の
[M5StackChan ライブラリ](https://github.com/m5stack/StackChan-BSP)（Arduino-ESP32 3.x 系）で首とカメラを制御する。
`M5StackChan.begin()` が `M5.begin()` を呼ぶので、`CoreS3.begin()` など M5CoreS3 ライブラリは使わない。
カメラは公式例と同じ GC0308 ラッパー（`src/gc0308.h`）で、VGA で初期化している。

**初回のみ: ホーム位置を合わせる。** 首の角度はホームからの相対値なので、
公式の Servo 例（Arduino 例の "set current position as home"）でカメラがケージ正面を向く姿勢をホームに設定しておく。
縦軸（ピッチ）は動かさない。M5Stack は縦軸を 5〜85° の範囲で使うよう推奨している。

```sh
cd firmware
cp src/secrets.h.example src/secrets.h   # Wi-Fi、トークン、UPLOAD_URL を記入
pio run -t upload
pio device monitor
```

起動時に1枚撮ってアップロードする（疎通確認）。画面は15秒で消え、タッチで再点灯する。

## 調整

`firmware/src/main.cpp` 冒頭のパラメータで調整する。

- 夜間停止の時間 → `QUIET_START_HOUR` / `QUIET_END_HOUR`。送信時刻は `worker/wrangler.toml` の `crons`（UTC）
- 中央合わせ → `CAMERA_HFOV_DEG`（水平画角、仮に60°）、`CENTER_DEADBAND`、`MAX_CENTER_YAW`、`MOVE_SPEED`。止めるなら `ENABLE_CENTERING = false`
- 猫の判定が厳しい/甘い → `worker/src/index.js` の `CAT_SCORE_THRESHOLD`（既定 0.6）
- 猫が動いても反応しない → `CELL_DIFF_THRESHOLD` か `MOTION_RATIO` を下げる
- 日差しや影で撮りすぎる → 上げる、または `MOTION_COOLDOWN_MS` を延ばす
- 輝度は RGB565 の緑成分（6bit）で近似している。2バイト平均だと緑の下位ビットのノイズで静止画面でも約25%のセルが変化扱いになり、誤検知が続いた
- 首を動かした後は `MOTION_SETTLE_MS`（既定2秒）だけ動体検知を止め、自動露出が落ち着いてから背景を取る

## 既知の注意

- TLS は `setInsecure()`（証明書検証なし）。気になる場合は Cloudflare のルート証明書を `setCACert` で固定する。
- 2026-10-25 に実機で Wi-Fi 接続・1時間ごとの撮影・首探索（猫を見つけて停止）・動体検知を確認した。
- CoreS3 は 2.4GHz の Wi-Fi にしか繋がらない。`WIFI_SSID` には 2.4GHz 側の SSID を入れる。
- `platformio.ini` のプラットフォームは `stable` の最新を取るため、PlatformIO Core が古いとビルドできないことがある（`pio upgrade` で解決）。
