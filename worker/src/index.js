const JST_OFFSET_MS = 9 * 3600 * 1000;
const MAX_UPLOAD_BYTES = 1024 * 1024;
const URL_LIFETIME_SEC = 30 * 24 * 3600;
const DIGEST_PHOTO_COUNT = 4;
const DETECTION_MODEL = "@cf/facebook/detr-resnet-50";
const CAT_SCORE_THRESHOLD = 0.6;

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

// 画像内で最もスコアの高い "cat" のスコアと、その枠の中心の横位置（0=左端, 1=右端）を返す。
// 猫がいなければ cx は null。判定に失敗したら cat は -1（不明）
export function bestCat(detections, width) {
  let best = null;
  for (const d of detections) {
    if (d.label === "cat" && (!best || d.score > best.score)) best = d;
  }
  if (!best) return { cat: 0, cx: null };
  const { xmin, xmax } = best.box || {};
  let cx = null;
  if (Number.isFinite(xmin) && Number.isFinite(xmax)) {
    // 座標が 0-1 の正規化値ならそのまま、ピクセルなら画像幅で割る
    const center = (xmin + xmax) / 2;
    cx = xmax <= 1 ? center : width > 0 ? center / width : null;
  }
  return { cat: best.score, cx };
}

async function detectCat(env, body, width) {
  try {
    const detections = await env.AI.run(DETECTION_MODEL, { image: [...new Uint8Array(body)] });
    return bestCat(detections, width);
  } catch (err) {
    console.error("cat detection failed", err);
    return { cat: -1, cx: null };
  }
}

async function handleUpload(request, env) {
  if (!authorized(request, env)) return new Response("unauthorized", { status: 401 });
  const params = new URL(request.url).searchParams;
  const kind = ["motion", "center"].includes(params.get("kind")) ? params.get("kind") : "hourly";
  const width = Number(params.get("w")) || 0;
  const body = await request.arrayBuffer();
  if (body.byteLength === 0 || body.byteLength > MAX_UPLOAD_BYTES) {
    return new Response("bad size", { status: 413 });
  }
  const head = new Uint8Array(body, 0, 2);
  if (head[0] !== 0xff || head[1] !== 0xd8) return new Response("not a jpeg", { status: 400 });

  const { cat, cx } = await detectCat(env, body, width);
  const { date, time } = jstNow();
  const key = `photos/${date}/${time}-${kind}.jpg`;
  await env.PHOTOS.put(key, body, {
    httpMetadata: { contentType: "image/jpeg" },
    customMetadata: { kind, cat: cat.toFixed(3) },
  });
  return Response.json({ key, cat, cx });
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
