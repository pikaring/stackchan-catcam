# Stack-chan 猫見守りカメラ

Stack-chan（M5Stack CoreS3）で猫を見守り、写真をクラウドに上げ、毎日4枚を母親にLINEで送る。

```
Stack-chan --HTTPS POST--> Cloudflare Worker --> R2 (写真保存)
  ・1時間ごと                    |
  ・動体検知                     +-- 毎日20:00 JST: 当日の4枚を選んでLINE Flexメッセージ1通で送信
```

- 動体検知はカメラ映像（VGA）を80x60セルに間引き、移動平均の背景との差分で判定する。
  60秒のクールダウンと1日150枚の上限あり。
- 首振り探索: 1時間ごとの撮影で猫スコアが低いと、首（ヨー軸）をホームから ±30°、±60° に振って撮影し、猫が見つかった位置で止まる。
  見つからなければ最もスコアの高かった向きに戻る。動体検知で撮った写真では探索しない。首が動いた後は動体検知の背景を取り直す。
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

動作確認（夜を待たずに送る）:

```sh
curl -X POST -H "Authorization: Bearer $UPLOAD_TOKEN" https://<worker-url>/digest
```

## 3. ファームウェア

対象は Kickstarter 版 StackChan（CoreS3 + SCS0009 サーボ）。公式の
[M5StackChan ライブラリ](https://github.com/m5stack/StackChan-BSP)（Arduino-ESP32 3.x 系）で首とカメラを制御する。
`M5StackChan.begin()` が `M5.begin()` を呼ぶので、`CoreS3.begin()` など M5CoreS3 ライブラリは使わない。
カメラは公式例と同じ GC0308 ラッパー（`src/gc0308.h`）で、VGA で初期化している。

**初回のみ: ホーム位置を合わせる。** 首探索の角度はホームからの相対値なので、
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

- 探索の角度・速度 → `SEARCH_YAW`（0.1度単位）、`SEARCH_SPEED`（0-1000）。探索を止めるなら `ENABLE_SEARCH = false`
- 猫の判定が厳しい/甘い → `worker/src/index.js` の `CAT_SCORE_THRESHOLD`（既定 0.6）
- 猫が動いても反応しない → `CELL_DIFF_THRESHOLD` か `MOTION_RATIO` を下げる
- 日差しや影で撮りすぎる → 上げる、または `MOTION_COOLDOWN_MS` を延ばす
- 輝度は RGB565 の2バイト平均で近似している。精度が必要なら緑成分の抽出に変える

## 既知の注意

- TLS は `setInsecure()`（証明書検証なし）。気になる場合は Cloudflare のルート証明書を `setCACert` で固定する。
- ファームウェアは**ビルドも実機確認もしていない**。PlatformIO の `lib_deps`（StackChan-BSP の git 指定、IRremoteESP8266、M5Unit-NFC のレジストリ名）は推測を含むので、通らなければ Arduino IDE の Library Manager で M5StackChan を入れる方法に切り替える。\n- 実機での動作確認、Workers AI による猫判定の精度確認は未実施。夜や暗い部屋だと判定が落ちる可能性がある。
