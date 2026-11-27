import Anthropic from "@anthropic-ai/sdk";

const JST_OFFSET_MS = 9 * 3600 * 1000;
const MAX_UPLOAD_BYTES = 1024 * 1024;
const URL_LIFETIME_SEC = 30 * 24 * 3600;
const DIGEST_PHOTO_COUNT = 4;
const DETECTION_MODEL = "@cf/facebook/detr-resnet-50";
const CAT_SCORE_THRESHOLD = 0.6;
// LINE の写真選びでは、人がこのスコア以上で写っている写真を後回しにする。
// 猫のそばに座った人が画面の大半を占める写真が、猫スコアだけで選ばれたことがあった (2026-10-31)
const PERSON_SCORE_THRESHOLD = 0.6;
// 首の向きを決めるための2つ目の判定 (Claude Haiku)。DETR は柵越しや影の猫を見落とすため。
// 見間違いもあるので LINE の写真選びには使わず、首を向ける (aim) だけに使う。
// Workers AI の LLM (Mistral / Llama 4 Scout) は猫のいない写真にも猫ありと答え、位置も当たらなかった (2026-10-27)
const VISION_MODEL = "claude-haiku-5-5";
const VISION_AIM_SCORE = 0.5; // LLM が猫ありと答えたときに返す aim (ファームの CAT_HINT_SCORE 以上)
const VISION_KINDS = ["hourly", "search", "center"];  // 動体検知の写真には使わない (回数を抑える)
const AIM_HINT_SCORE = 0.3;   // ファームの CAT_HINT_SCORE と揃える。DETR がこれ未満のときだけ LLM に聞く
const VISION_TIMEOUT_MS = 10000;
// 例に数値を書くと、そのまま写して返すモデルがあったので書かない
const VISION_PROMPT =
  "This is a photo from a pet camera in a room with a wire cat cage. " +
  "Is there a cat anywhere in the image? Cats may be partly hidden behind cage bars, in shadow, or on a shelf. " +
  "If there is a cat, give the center of its body as fractions of the image size: " +
  "x from 0 (left edge) to 1 (right edge), y from 0 (top edge) to 1 (bottom edge). " +
  'Reply with only JSON: {"cat": false} or {"cat": true, "x": <number>, "y": <number>}.';

// 18:05 の写真選びで、人が写っていないか・本当に猫が写っているかを Haiku に聞く。
// 物体検出 (DETR) は後ろ姿や写真の端の袖・腕を人と判定できず (2026-11-01)、壁や暗い棚を猫と判定することもある。
// 2026-11-02 に15枚で試すと、後ろ姿・端の袖・腕だけの写真と、猫と誤判定された2枚も含めて全部当たった
const PHOTO_CHECK_PROMPT =
  "This is a photo from a pet camera in a room. Answer two questions. " +
  "1) Is any part of a human visible, such as a person seen from behind, hair, a back, an arm, a hand, a leg, or a foot? " +
  "Ignore pictures of people on posters, boxes, or screens. " +
  "2) Is a real cat clearly visible? Plush toys and pictures of cats do not count. " +
  'Reply with only JSON: {"person": true or false, "cat": true or false}.';
const PHOTO_CHECKS_PER_SLOT = 3; // 1区間で Haiku に聞く写真の上限 (猫スコアの高い順)
// 18:05 は区間に分けず、その日いちばんよく撮れた写真を送る (2026-11-11 ユーザー判断)。
// PHOTO_CHECK_PROMPT の2問に、写りの点 (1〜10) を足して Haiku に聞く
const PHOTO_RATE_PROMPT =
  "This is a photo from a pet camera in a room. Answer three questions. " +
  "1) Is any part of a human visible, such as a person seen from behind, hair, a back, an arm, a hand, a leg, or a foot? " +
  "Ignore pictures of people on posters, boxes, or screens. " +
  "2) Is a real cat clearly visible? Plush toys and pictures of cats do not count. " +
  "3) How good is this as a photo of the cat, as an integer from 1 (poor) to 10 (excellent)? " +
  "Rate higher when the cat is large in the frame, its face and eyes are visible, it is sharp, and it is well lit. " +
  "Rate lower when it is blurry, dark, far away, cut off at the edge, or mostly hidden behind cage bars. " +
  "4) Estimate the cat's mood from its face, posture, and movement as probabilities from 0 to 1 that sum to 1: " +
  "relaxed, alert, scared, aggressive. " +
  "5) Is the photo good enough to judge the mood? Answer false if the light is poor, the cat's face or body is hard to see, " +
  "it is blurry, far away, cut off, hidden behind bars, or seen from a bad angle. " +
  'Reply with only flat JSON: {"person": true or false, "cat": true or false, "quality": <integer>, ' +
  '"relaxed": <number>, "alert": <number>, "scared": <number>, "aggressive": <number>, "mood_clear": true or false}.';
// 気分の推定 (MiaoSense の4分類にならう)。写真から Claude Haiku が見た目で推定する目安で、診断ではない
export const MOODS = {
  relaxed: { label: "リラックス", emoji: "😌", color: "#2E9E5B" },
  alert: { label: "警戒", emoji: "👀", color: "#E0A100" },
  scared: { label: "怖がり", emoji: "😨", color: "#3B7DD8" },
  aggressive: { label: "攻撃的", emoji: "😾", color: "#D64545" },
};
const MOOD_MIN_CONFIDENCE = 0.5; // いちばん高い気分がこれ未満なら「気分が不明瞭です」にする
const RATE_CANDIDATES = 12;          // Haiku に点を付けさせる写真の上限 (猫スコアの高い順)
const RATE_CANDIDATES_PER_HOUR = 2;  // 探す・寄せるで続けて撮った似た写真ばかりが候補にならないよう、1時間ごとの上限
const DIGEST_MIN_GAP_MIN = 60;       // 送る写真どうしをこの分数以上離す (似た写真が並ばないように)

const encoder = new TextEncoder();

function jstNow(date = new Date()) {
  const iso = new Date(date.getTime() + JST_OFFSET_MS).toISOString();
  return { date: iso.slice(0, 10), time: iso.slice(11, 19).replace(/:/g, "") };
}

function toHex(buf) {
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function hmacKey(secret, usage) {
  return crypto.subtle.importKey("raw", encoder.encode(secret), { name: "HMAC", hash: "SHA-256" }, false, [usage]);
}

async function sign(env, key, exp) {
  const k = await hmacKey(env.SIGNING_SECRET, "sign");
  return toHex(await crypto.subtle.sign("HMAC", k, encoder.encode(`${key}.${exp}`)));
}

async function signedUrl(env, key) {
  const exp = Math.floor(Date.now() / 1000) + URL_LIFETIME_SEC;
  const sig = await sign(env, key, exp);
  return `${env.PUBLIC_BASE_URL}/img/${key}?exp=${exp}&sig=${sig}`;
}

async function verifySig(env, key, exp, sig) {
  if (!exp || !sig || Number(exp) < Date.now() / 1000) return false;
  const expected = await sign(env, key, exp);
  return expected.length === sig.length && timingSafeEqual(expected, sig);
}

function timingSafeEqual(a, b) {
  const ab = encoder.encode(a);
  const bb = encoder.encode(b);
  if (ab.length !== bb.length) return false;
  return crypto.subtle.timingSafeEqual(ab, bb);
}

function authorized(request, env) {
  const header = request.headers.get("Authorization") || "";
  return env.UPLOAD_TOKEN && timingSafeEqual(header, `Bearer ${env.UPLOAD_TOKEN}`);
}

// 画像内で最もスコアの高い "cat" のスコアと、首を向ける猫の枠の中心の位置を返す。
// 首を向けるのは AIM_HINT_SCORE 以上の猫のうち、枠がいちばん大きいもの。遠くの棚のぬいぐるみが
// 本物の猫より高いスコアで猫と判定され、首が本物の猫から離れたことがあった (2026-10-28)。近くの本物の猫は大きく写る。
// cx は横 (0=左端, 1=右端)、cy は縦 (0=上端, 1=下端)。猫がいなければ null。判定に失敗したら cat は -1（不明）
// person は "person" の最高スコア (LINE の写真選びで人の写った写真を後回しにするため)
export function bestCat(detections, width, height) {
  let best = null;
  let biggest = null;
  let person = 0;
  for (const d of detections) {
    if (d.label === "person") person = Math.max(person, d.score);
    if (d.label !== "cat") continue;
    if (!best || d.score > best.score) best = d;
    if (d.score >= AIM_HINT_SCORE && (!biggest || boxArea(d.box) > boxArea(biggest.box))) biggest = d;
  }
  if (!best) return { cat: 0, cx: null, cy: null, person };
  const { xmin, xmax, ymin, ymax } = (biggest || best).box || {};
  return { cat: best.score, cx: boxCenter(xmin, xmax, width), cy: boxCenter(ymin, ymax, height), person };
}

function boxArea(box) {
  const { xmin, xmax, ymin, ymax } = box || {};
  const area = (xmax - xmin) * (ymax - ymin);
  return Number.isFinite(area) ? area : 0;
}

// 座標が 0-1 の正規化値ならそのまま、ピクセルなら画像の幅 (高さ) で割る
function boxCenter(min, max, size) {
  if (!Number.isFinite(min) || !Number.isFinite(max)) return null;
  const center = (min + max) / 2;
  return max <= 1 ? center : size > 0 ? center / size : null;
}

async function detectCat(env, body, width, height) {
  try {
    const detections = await env.AI.run(DETECTION_MODEL, { image: [...new Uint8Array(body)] });
    return bestCat(detections, width, height);
  } catch (err) {
    console.error("cat detection failed", err);
    return { cat: -1, cx: null, cy: null, person: null };
  }
}

// 起動後最初のアップロードに付く再起動の情報 (リセット理由など) をメタデータ用に取り出す
const BOOT_INFO_KEYS = ["reset", "boots", "heap", "wifi"];
export function bootInfo(params) {
  const info = {};
  for (const k of BOOT_INFO_KEYS) {
    const v = params.get(k);
    if (v && /^[A-Za-z0-9_]{1,16}$/.test(v)) info[k] = v;
  }
  return info;
}

// LLM の返答から {x, y} を取り出す。猫なし・読めなければ null
export function parseVision(text) {
  const m = typeof text === "string" ? text.match(/\{[^}]*\}/) : null;
  if (!m) return null;
  try {
    const j = JSON.parse(m[0]);
    if (j.cat !== true) return null;
    const unit = (v) => (Number.isFinite(Number(v)) && Number(v) >= 0 && Number(v) <= 1 ? Number(v) : null);
    return { x: unit(j.x), y: unit(j.y) };
  } catch {
    return null;
  }
}

// Haiku の答えから「送ってよい写真か」(猫あり、avoidPeople なら人なしも) を取り出す。読めなければ null
export function parsePhotoCheck(text, avoidPeople = true) {
  const m = typeof text === "string" ? text.match(/\{[^}]*\}/) : null;
  if (!m) return null;
  try {
    const j = JSON.parse(m[0]);
    if (typeof j.person !== "boolean" || typeof j.cat !== "boolean") return null;
    return avoidPeople ? !j.person && j.cat : j.cat;
  } catch {
    return null;
  }
}

// 4つの気分の確率から { mood, confidence } を取り出す。条件が悪い・確率が読めない・確信が低いときは mood が null (不明瞭)。
// 確率は合計が1になるよう直す
export function parseMood(j) {
  if (j.mood_clear !== true) return { mood: null, confidence: 0 };
  const probs = Object.keys(MOODS).map((k) => [k, Number(j[k])]);
  if (probs.some(([, v]) => !Number.isFinite(v) || v < 0)) return { mood: null, confidence: 0 };
  const total = probs.reduce((sum, [, v]) => sum + v, 0);
  if (total <= 0) return { mood: null, confidence: 0 };
  const [mood, top] = probs.reduce((a, b) => (b[1] > a[1] ? b : a));
  const confidence = top / total;
  return confidence >= MOOD_MIN_CONFIDENCE ? { mood, confidence } : { mood: null, confidence };
}

// Haiku の写りの採点から { ok: 送ってよいか, quality: 1〜10, mood, confidence } を取り出す。読めなければ null
// 気分だけ読めなくても写真選びは止めない (mood は null = 不明瞭)
export function parsePhotoRating(text, avoidPeople = true) {
  const ok = parsePhotoCheck(text, avoidPeople);
  if (ok === null) return null;
  const j = JSON.parse(text.match(/\{[^}]*\}/)[0]);
  const quality = Number(j.quality);
  if (!Number.isFinite(quality) || quality < 1 || quality > 10) return null;
  return { ok, quality, ...parseMood(j) };
}

async function askVision(env, body) {
  const text = await askHaiku(env, body, VISION_PROMPT);
  return text === null ? { answered: false, cat: null } : { answered: true, cat: parseVision(text) };
}

// R2 の写真が送ってよいもの (猫あり、avoidPeople なら人なしも) かを Haiku に聞く。true / false、答えられなければ null
async function checkPhoto(env, key, avoidPeople = true) {
  const object = await env.PHOTOS.get(key);
  if (!object) return null;
  return parsePhotoCheck(await askHaiku(env, await object.arrayBuffer(), PHOTO_CHECK_PROMPT), avoidPeople);
}

// R2 の写真を Haiku に採点させる。{ ok, quality }、答えられなければ null
async function ratePhoto(env, key, avoidPeople = true) {
  const object = await env.PHOTOS.get(key);
  if (!object) return null;
  return parsePhotoRating(await askHaiku(env, await object.arrayBuffer(), PHOTO_RATE_PROMPT), avoidPeople);
}

// 画像と質問を Haiku に送り、答えの文字列を返す。キーがない・拒否・エラーなら null
async function askHaiku(env, body, prompt) {
  if (!env.ANTHROPIC_API_KEY) return null;
  const client = new Anthropic({ apiKey: env.ANTHROPIC_API_KEY, timeout: VISION_TIMEOUT_MS, maxRetries: 0 });
  const data = btoa(Array.from(new Uint8Array(body), (c) => String.fromCharCode(c)).join(""));
  try {
    const msg = await client.messages.create({
      model: VISION_MODEL,
      max_tokens: 1024,
      output_config: { effort: "low" },
      messages: [
        {
          role: "user",
          content: [
            { type: "image", source: { type: "base64", media_type: "image/jpeg", data } },
            { type: "text", text: prompt },
          ],
        },
      ],
    });
    if (msg.stop_reason === "refusal") return null;
    // 答えの前に説明文が付くことがあるので、呼び出し側が JSON の部分だけを読む
    return msg.content.filter((b) => b.type === "text").map((b) => b.text).join("");
  } catch (err) {
    console.error("haiku check failed", err instanceof Anthropic.APIError ? `${err.status} ${err.message}` : err);
    return null;
  }
}

async function handleUpload(request, env) {
  if (!authorized(request, env)) return new Response("unauthorized", { status: 401 });
  const params = new URL(request.url).searchParams;
  const kind = ["motion", "center", "search"].includes(params.get("kind")) ? params.get("kind") : "hourly";
  const width = Number(params.get("w")) || 0;
  const height = Number(params.get("h")) || 0;
  const body = await request.arrayBuffer();
  if (body.byteLength === 0 || body.byteLength > MAX_UPLOAD_BYTES) {
    return new Response("bad size", { status: 413 });
  }
  const head = new Uint8Array(body, 0, 2);
  if (head[0] !== 0xff || head[1] !== 0xd8) return new Response("not a jpeg", { status: 400 });

  const { cat, cx, cy, person } = await detectCat(env, body, width, height);
  // aim/aimCx/aimCy は首の向きを決めるためだけの値。cat (DETR) は LINE の写真選びに使う
  let aim = cat;
  let aimCx = cx;
  let aimCy = cy;
  const extra = {};
  if (person !== null) extra.person = person.toFixed(3);
  if (cat >= 0 && cat < AIM_HINT_SCORE && VISION_KINDS.includes(kind)) {
    const vision = await askVision(env, body);
    if (vision.answered) extra.vision = vision.cat ? "cat" : "none";
    if (vision.cat) {
      aim = VISION_AIM_SCORE;
      aimCx = vision.cat.x;
      aimCy = vision.cat.y;
      if (aimCx !== null) extra.vx = aimCx.toFixed(2);
      if (aimCy !== null) extra.vy = aimCy.toFixed(2);
    }
  }
  const { date, time } = jstNow();
  const key = `photos/${date}/${time}-${kind}.jpg`;
  await env.PHOTOS.put(key, body, {
    httpMetadata: { contentType: "image/jpeg" },
    customMetadata: { kind, cat: cat.toFixed(3), ...extra, ...bootInfo(params) },
  });
  return Response.json({ key, cat, cx, cy, aim, aimCx, aimCy });
}

async function handleImage(request, env, key) {
  const url = new URL(request.url);
  if (!(await verifySig(env, key, url.searchParams.get("exp"), url.searchParams.get("sig")))) {
    return new Response("forbidden", { status: 403 });
  }
  const object = await env.PHOTOS.get(key);
  if (!object) return new Response("not found", { status: 404 });
  return new Response(object.body, {
    headers: { "Content-Type": "image/jpeg", "Cache-Control": "private, max-age=86400" },
  });
}

export function catScoreOf(object) {
  const score = Number(object.customMetadata?.cat);
  return Number.isFinite(score) ? score : -1;
}

// 人が写っているか。person の記録がない写真 (2026-10-31 より前) は人なしとみなす
export function hasPerson(object) {
  const score = Number(object.customMetadata?.person);
  return Number.isFinite(score) && score >= PERSON_SCORE_THRESHOLD;
}

// 猫が写っている写真だけを対象に、時系列で4等分した各区間から猫スコア最大の1枚を選ぶ。
// avoidPeople なら、区間に人の写っていない写真があれば、その中から選ぶ (なければ人ありからも選ぶ)
export function pickPhotos(objects, count = DIGEST_PHOTO_COUNT, threshold = CAT_SCORE_THRESHOLD, avoidPeople = true) {
  const person = avoidPeople ? hasPerson : () => false;
  const cats = catPhotos(objects, threshold);
  if (cats.length <= count) {
    const noPerson = cats.filter((o) => !person(o));
    return noPerson.length > 0 ? noPerson : cats;
  }
  // 人なしを優先し、どちらも同じなら猫スコアの高い方
  const better = (a, b) => {
    if (person(a) !== person(b)) return person(a) ? b : a;
    return catScoreOf(b) > catScoreOf(a) ? b : a;
  };
  const picks = [];
  for (let i = 0; i < count; i++) {
    const from = Math.floor((i * cats.length) / count);
    const to = Math.floor(((i + 1) * cats.length) / count);
    picks.push(cats.slice(from, to).reduce(better));
  }
  return picks;
}

function catPhotos(objects, threshold = CAT_SCORE_THRESHOLD) {
  return objects.filter((o) => catScoreOf(o) >= threshold).sort((a, b) => (a.key < b.key ? -1 : 1));
}

// 18:05 の写真選び。pickPhotos と同じ区間ごとに、猫スコアの高い順に Haiku に確かめ、
// 最初に「人なしで猫あり」と答えた写真を選ぶ。聞いた写真がどれも駄目な区間は飛ばす (送る枚数が減る)。
// どの区間も選べないとき、または Haiku が答えられないときは pickPhotos の結果を使う。
// check は (key) => true (送ってよい) / false / null (答えられない)。テストで差し替えられるよう引数にしている
export async function pickCheckedPhotos(objects, check, count = DIGEST_PHOTO_COUNT, avoidPeople = true) {
  const fallback = pickPhotos(objects, count, CAT_SCORE_THRESHOLD, avoidPeople);
  const cats = catPhotos(objects);
  const slots = [];
  if (cats.length <= count) {
    for (const o of cats) slots.push([o]);
  } else {
    for (let i = 0; i < count; i++) {
      slots.push(cats.slice(Math.floor((i * cats.length) / count), Math.floor(((i + 1) * cats.length) / count)));
    }
  }
  const picks = [];
  for (const slot of slots) {
    const candidates = slot
      .filter((o) => !avoidPeople || !hasPerson(o))
      .sort((a, b) => catScoreOf(b) - catScoreOf(a))
      .slice(0, PHOTO_CHECKS_PER_SLOT);
    let chosen = null;
    for (const o of candidates) {
      const result = await check(o.key);
      if (result === null) return fallback; // Haiku が答えられないときは物体検出の結果だけで選ぶ
      // check は true / false か、気分つきの { ok, mood, confidence } を返す
      const ok = typeof result === "object" ? result.ok : result;
      if (ok) {
        if (typeof result === "object") o.moodResult = { mood: result.mood, confidence: result.confidence };
        chosen = o;
        break;
      }
    }
    picks.push(chosen);
  }
  const kept = picks.filter((o) => o);
  return kept.length > 0 ? kept : fallback;
}

// key (photos/<日付>/HHMMSS-<kind>.jpg) の撮った時刻を分で返す。18:05 は前日分もまたぐので日付も含める
function minuteOf(key) {
  return Date.parse(key.slice(7, 17)) / 60000 + Number(key.slice(18, 20)) * 60 + Number(key.slice(20, 22));
}

// 18:05 の写真選び (digestRange の24時間分)。猫スコアの高い写真 (1時間に2枚まで、合わせて12枚まで) を Haiku に採点させ、
// 「人なしで猫あり」の中から点の高い順に、すでに選んだ写真と1時間以上離れたものを count 枚まで選ぶ。
// 送るときは時間順に並べる。どれも選べないとき、または Haiku が答えられないときは pickPhotos の結果を使う。
// rate は (key) => { ok, quality } / null (答えられない)。テストで差し替えられるよう引数にしている
export async function pickBestPhotos(objects, rate, count = DIGEST_PHOTO_COUNT, avoidPeople = true) {
  const fallback = pickPhotos(objects, count, CAT_SCORE_THRESHOLD, avoidPeople);
  const perHour = new Map();
  const candidates = [];
  for (const o of catPhotos(objects)
    .filter((o) => !avoidPeople || !hasPerson(o))
    .sort((a, b) => catScoreOf(b) - catScoreOf(a))) {
    const hour = o.key.slice(7, 20); // 日付/時
    if ((perHour.get(hour) ?? 0) >= RATE_CANDIDATES_PER_HOUR) continue;
    perHour.set(hour, (perHour.get(hour) ?? 0) + 1);
    candidates.push(o);
    if (candidates.length >= RATE_CANDIDATES) break;
  }
  const ratings = await Promise.all(candidates.map((o) => rate(o.key)));
  if (ratings.some((r) => r === null)) return fallback; // Haiku が答えられないときは物体検出の結果だけで選ぶ
  const ranked = candidates
    .map((o, i) => ({ o, ...ratings[i] }))
    .filter((c) => c.ok)
    .sort((a, b) => b.quality - a.quality || catScoreOf(b.o) - catScoreOf(a.o));
  const picks = [];
  for (const { o, mood, confidence } of ranked) {
    if (picks.length >= count) break;
    if (picks.every((p) => Math.abs(minuteOf(p.key) - minuteOf(o.key)) >= DIGEST_MIN_GAP_MIN)) {
      o.moodResult = { mood, confidence }; // LINE の写真に添える気分 (buildFlex に渡す)
      picks.push(o);
    }
  }
  picks.sort((a, b) => (a.key < b.key ? -1 : 1));
  return picks.length > 0 ? picks : fallback;
}

function imageBox(url) {
  return {
    type: "image",
    url,
    size: "full",
    aspectMode: "cover",
    aspectRatio: "4:3",
    flex: 1,
    action: { type: "uri", uri: url },
  };
}

// 気分の表示。mood が undefined (気分を調べていない) なら null、mood が null なら「気分が不明瞭です」
export function moodCaption(result) {
  if (!result) return null;
  const m = result.mood && MOODS[result.mood];
  if (!m) return { text: "❔ 気分が不明瞭です", color: "#9AA0A6" };
  return { text: `${m.emoji} ${m.label} ${Math.round(result.confidence * 100)}%`, color: m.color };
}

// 気分があれば、写真を気分の色の枠で囲み、下に気分と確からしさを添える
function photoCell(url, result) {
  const caption = moodCaption(result);
  if (!caption) return imageBox(url);
  return {
    type: "box",
    layout: "vertical",
    flex: 1,
    spacing: "xs",
    paddingAll: "xs",
    cornerRadius: "md",
    borderWidth: "medium",
    borderColor: caption.color,
    contents: [
      imageBox(url),
      { type: "text", text: caption.text, size: "xs", weight: "bold", color: caption.color, align: "center", wrap: true },
    ],
  };
}

// moods は urls と同じ順の { mood, confidence } の配列 (省略すると気分を添えない)
export function buildFlex(date, urls, title = `今日の猫 ${date}`, moods = []) {
  const rows = [];
  for (let i = 0; i < urls.length; i += 2) {
    rows.push({
      type: "box",
      layout: "horizontal",
      spacing: "sm",
      contents: urls.slice(i, i + 2).map((url, j) => photoCell(url, moods[i + j])),
    });
  }
  if (moods.some(Boolean)) {
    rows.push({ type: "text", text: "気分は写真から AI が見た目で推定した目安です", size: "xxs", color: "#9AA0A6", wrap: true });
  }
  return {
    type: "flex",
    altText: title,
    contents: {
      type: "bubble",
      size: "mega",
      header: {
        type: "box",
        layout: "vertical",
        contents: [{ type: "text", text: `🐱 ${title}`, weight: "bold" }],
      },
      body: { type: "box", layout: "vertical", spacing: "sm", contents: rows },
    },
  };
}

// to が空なら友だち全員へ broadcast
async function lineSend(env, message, to = env.LINE_TO) {
  const endpoint = to ? "push" : "broadcast";
  const payload = to ? { to, messages: [message] } : { messages: [message] };
  const res = await fetch(`https://api.line.me/v2/bot/message/${endpoint}`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Authorization: `Bearer ${env.LINE_CHANNEL_ACCESS_TOKEN}`,
    },
    body: JSON.stringify(payload),
  });
  if (!res.ok) throw new Error(`LINE ${endpoint} failed: ${res.status} ${await res.text()}`);
}

// to を渡すとその人だけへ送る (確認用。POST /digest?owner=1 で持ち主へ)
// 18:05 の対象は前日 18:05 から。当日だけだと、猫のよく写る夕方〜夜 (18:05 以降) の写真がどの日にも入らなかった (2026-11-17)
export function digestRange(now = new Date()) {
  const today = jstNow(now);
  const yesterday = jstNow(new Date(now.getTime() - 24 * 3600 * 1000));
  return { date: today.date, dates: [yesterday.date, today.date], fromKey: `${yesterday.date}/180500` };
}

async function sendDigest(env, to = env.LINE_TO) {
  const { date, dates, fromKey } = digestRange();
  let objects = [];
  for (const d of dates) {
    const listed = await env.PHOTOS.list({ prefix: `photos/${d}/`, include: ["customMetadata"] });
    objects = objects.concat(listed.objects);
  }
  objects = objects.filter((o) => o.key.slice("photos/".length) >= fromKey);
  // 6:00・12:00 に持ち主へ送った写真も含め、全部から選ぶ。持ち主への送信は確認用で、18:05 は全員へ送るため (2026-11-11)
  // 人が写っていても選ぶ (avoidPeople = false)。人を避けるのをやめた (2026-11-25)
  const picks = await pickBestPhotos(objects, (key) => ratePhoto(env, key, false), DIGEST_PHOTO_COUNT, false);
  if (picks.length === 0) {
    await lineSend(env, { type: "text", text: `${date} は猫が写った写真がありませんでした。` }, to);
    return { date, sent: 0 };
  }
  const urls = await Promise.all(picks.map((o) => signedUrl(env, o.key)));
  await lineSend(env, buildFlex(date, urls, undefined, picks.map((o) => o.moodResult)), to);
  return { date, sent: picks.length };
}

// 6:00 と 12:00 JST に、持ち主 (LINE_OWNER_ID) だけへ途中経過の写真を送る。人が写っていてもよい。
// 6:00 は前日 12:00 から、12:00 は当日 6:00 からの写真が対象。Mac がスリープしていても届くよう Worker から送る
export const OWNER_REPORT_CRONS = { "0 21 * * *": 6, "0 3 * * *": 12 };

export function ownerReportRange(now = new Date()) {
  const today = jstNow(now);
  const hour = Number(today.time.slice(0, 2));
  if (hour < 12) {
    const yesterday = jstNow(new Date(now.getTime() - 24 * 3600 * 1000));
    return { title: "朝の猫", dates: [yesterday.date, today.date], fromKey: `${yesterday.date}/120000` };
  }
  return { title: "昼の猫", dates: [today.date], fromKey: `${today.date}/060000` };
}

async function sendOwnerReport(env, now = new Date()) {
  if (!env.LINE_OWNER_ID) return { sent: 0, reason: "LINE_OWNER_ID is not set" };
  const { title, dates, fromKey } = ownerReportRange(now);
  let objects = [];
  for (const date of dates) {
    const listed = await env.PHOTOS.list({ prefix: `photos/${date}/`, include: ["customMetadata"] });
    objects = objects.concat(listed.objects);
  }
  objects = objects.filter((o) => o.key.slice("photos/".length) >= fromKey);
  // 写りの点は使わないが、気分を添えるため、18:05 と同じ採点の質問で確かめる
  const picks = await pickCheckedPhotos(objects, (key) => ratePhoto(env, key, false), DIGEST_PHOTO_COUNT, false);
  if (picks.length === 0) {
    const last = objects.map((o) => o.key).sort().pop();
    const text = last
      ? `${title}: 猫が写った写真なし（写真 ${objects.length} 枚、最後は ${last.slice(7, 24)}）`
      : `${title}: 写真が1枚もありません。カメラが止まっているかもしれません（電源・Wi-Fi を確認）`;
    await lineSend(env, { type: "text", text }, env.LINE_OWNER_ID);
    return { sent: 0, photos: objects.length };
  }
  const urls = await Promise.all(picks.map((o) => signedUrl(env, o.key)));
  const times = picks.map((o) => `${o.key.slice(18, 20)}:${o.key.slice(20, 22)}`).join(" ");
  await lineSend(env, buildFlex(dates.at(-1), urls, `${title} ${times}`, picks.map((o) => o.moodResult)), env.LINE_OWNER_ID);
  return { sent: picks.length, photos: objects.length };
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "POST" && url.pathname === "/upload") return handleUpload(request, env);
    if (request.method === "GET" && url.pathname.startsWith("/img/")) {
      return handleImage(request, env, decodeURIComponent(url.pathname.slice("/img/".length)));
    }
    if (request.method === "POST" && url.pathname === "/digest") {
      if (!authorized(request, env)) return new Response("unauthorized", { status: 401 });
      if (url.searchParams.get("owner") === "1" && !env.LINE_OWNER_ID) return new Response("LINE_OWNER_ID is not set", { status: 400 });
      return Response.json(await sendDigest(env, url.searchParams.get("owner") === "1" ? env.LINE_OWNER_ID : env.LINE_TO));
    }
    if (request.method === "POST" && url.pathname === "/owner-report") {
      if (!authorized(request, env)) return new Response("unauthorized", { status: 401 });
      return Response.json(await sendOwnerReport(env));
    }
    return new Response("not found", { status: 404 });
  },

  async scheduled(event, env, ctx) {
    ctx.waitUntil(event.cron in OWNER_REPORT_CRONS ? sendOwnerReport(env) : sendDigest(env));
  },
};
