import Anthropic from "@anthropic-ai/sdk";

const JST_OFFSET_MS = 9 * 3600 * 1000;
const MAX_UPLOAD_BYTES = 1024 * 1024;
const URL_LIFETIME_SEC = 30 * 24 * 3600;
const DIGEST_PHOTO_COUNT = 4;
const DETECTION_MODEL = "@cf/facebook/detr-resnet-50";
const CAT_SCORE_THRESHOLD = 0.6;
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
export function bestCat(detections, width, height) {
  let best = null;
  let biggest = null;
  for (const d of detections) {
    if (d.label !== "cat") continue;
    if (!best || d.score > best.score) best = d;
    if (d.score >= AIM_HINT_SCORE && (!biggest || boxArea(d.box) > boxArea(biggest.box))) biggest = d;
  }
  if (!best) return { cat: 0, cx: null, cy: null };
  const { xmin, xmax, ymin, ymax } = (biggest || best).box || {};
  return { cat: best.score, cx: boxCenter(xmin, xmax, width), cy: boxCenter(ymin, ymax, height) };
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
    return { cat: -1, cx: null, cy: null };
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

async function askVision(env, body) {
  if (!env.ANTHROPIC_API_KEY) return { answered: false, cat: null };
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
            { type: "text", text: VISION_PROMPT },
          ],
        },
      ],
    });
    if (msg.stop_reason === "refusal") return { answered: false, cat: null };
    // 答えの前に説明文が付くことがあるので、parseVision が JSON の部分だけを読む
    const text = msg.content.filter((b) => b.type === "text").map((b) => b.text).join("");
    return { answered: true, cat: parseVision(text) };
  } catch (err) {
    console.error("vision check failed", err instanceof Anthropic.APIError ? `${err.status} ${err.message}` : err);
    return { answered: false, cat: null };
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

  const { cat, cx, cy } = await detectCat(env, body, width, height);
  // aim/aimCx/aimCy は首の向きを決めるためだけの値。cat (DETR) は LINE の写真選びに使う
  let aim = cat;
  let aimCx = cx;
  let aimCy = cy;
  const extra = {};
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

// 猫が写っている写真だけを対象に、時系列で4等分した各区間から猫スコア最大の1枚を選ぶ
export function pickPhotos(objects, count = DIGEST_PHOTO_COUNT, threshold = CAT_SCORE_THRESHOLD) {
  const cats = objects
    .filter((o) => catScoreOf(o) >= threshold)
    .sort((a, b) => (a.key < b.key ? -1 : 1));
  if (cats.length <= count) return cats;
  const picks = [];
  for (let i = 0; i < count; i++) {
    const from = Math.floor((i * cats.length) / count);
    const to = Math.floor(((i + 1) * cats.length) / count);
    const best = cats.slice(from, to).reduce((a, b) => (catScoreOf(b) > catScoreOf(a) ? b : a));
    picks.push(best);
  }
  return picks;
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

export function buildFlex(date, urls) {
  const rows = [];
  for (let i = 0; i < urls.length; i += 2) {
    rows.push({
      type: "box",
      layout: "horizontal",
      spacing: "sm",
      contents: urls.slice(i, i + 2).map(imageBox),
    });
  }
  return {
    type: "flex",
    altText: `今日の猫（${date}）`,
    contents: {
      type: "bubble",
      size: "mega",
      header: {
        type: "box",
        layout: "vertical",
        contents: [{ type: "text", text: `🐱 今日の猫 ${date}`, weight: "bold" }],
      },
      body: { type: "box", layout: "vertical", spacing: "sm", contents: rows },
    },
  };
}

async function lineSend(env, message) {
  const endpoint = env.LINE_TO ? "push" : "broadcast";
  const payload = env.LINE_TO ? { to: env.LINE_TO, messages: [message] } : { messages: [message] };
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

async function sendDigest(env) {
  const { date } = jstNow();
  const listed = await env.PHOTOS.list({ prefix: `photos/${date}/`, include: ["customMetadata"] });
  const picks = pickPhotos(listed.objects);
  if (picks.length === 0) {
    await lineSend(env, { type: "text", text: `${date} は猫が写った写真がありませんでした。` });
    return { date, sent: 0 };
  }
  const urls = await Promise.all(picks.map((o) => signedUrl(env, o.key)));
  await lineSend(env, buildFlex(date, urls));
  return { date, sent: picks.length };
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
      return Response.json(await sendDigest(env));
    }
    return new Response("not found", { status: 404 });
  },

  async scheduled(_event, env, ctx) {
    ctx.waitUntil(sendDigest(env));
  },
};
