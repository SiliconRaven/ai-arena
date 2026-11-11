/* The replay player: draws an AI Smash replay on a canvas (the stage from the side, each fighter's little figure, hits,
   shots and KOs), with the fighters on the left and the KO feed on the right. The replay is embedded in the page and can also be loaded from a file.

   Effects are computed from the replay's events at the moment T, never accumulated, so scrubbing shows the same
   picture as playing. Hash parameters for a fixed picture: #t=TURN&speed=N&play=1&ui=0&z=ZOOM&c=X,Y. */
(function () {
  "use strict";
  const Art = window.DungeonArt;
  const $ = id => document.getElementById(id);
  const esc = s => String(s).replace(/[&<>"']/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));
  const canvas = $("stage"), ctx = canvas.getContext("2d");
  let dirty = true, R = null;
  const sprites = Art.Sprites(() => { dirty = true; });
  const view = {T: 0, playing: false, speed: 1, last: 0, cam: {x: 0, y: 5, s: 20}, free: false, feed: -1, card: ""};
  const INK = "#2b2340", FIGURE_HEIGHT = 3.1;
  const ISLAND_TOP = 48, DECK_TOP = 16;                    // where the walkable surface is in stage/island.png and stage/platform.png

  // ---- loading ----
  async function bytesToReplay(buffer) {
    let bytes = new Uint8Array(buffer);
    if (bytes[0] === 0x1f && bytes[1] === 0x8b) {
      const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
      bytes = new Uint8Array(await new Response(stream).arrayBuffer());
    }
    const text = new TextDecoder("utf-8").decode(bytes).trim();
    if (text.startsWith("<")) {
      const embedded = text.match(/\bconst REPLAY=(\{[\s\S]*?\});[ \t]*\r?\n[ \t]*const COLORS\s*=/);
      if (!embedded) throw new Error("这份 HTML 里没有可读取的回放数据");
      return JSON.parse(embedded[1]);
    }
    return JSON.parse(text);
  }
  function picker(message) {
    $("picker").hidden = false;
    if (message) $("load-error").textContent = message;
  }
  async function readFile(file) {
    try { open(await bytesToReplay(await file.arrayBuffer())); $("picker").hidden = true; }
    catch (error) { picker(`读不了这个文件：${error.message}`); }
  }

  // ---- the replay as ticks ----
  function index(replay) {
    const header = replay.header, frames = replay.frames;
    if (!String(header?.rules?.version).startsWith("sm-") || !frames?.length) {
      throw new Error("请选择 AI Smash 的回放文件");
    }
    const pos = [{p: frames[0].players.map(s => s.out ? null : [s.x, s.y, s.vx, s.vy, s.facing]), s: []}], frameOf = [0], byTick = new Map();
    frames.forEach((frame, k) => {
      for (const t of frame.ticks || []) { pos.push(t); frameOf.push(k); }
      for (const e of frame.events || []) (byTick.get(e.tick) || byTick.set(e.tick, []).get(e.tick)).push(e);
    });
    const names = header.names || [];
    const colors = replay.presentation?.colors || [];
    const seats = frames[0].players.map((_, i) => ({i, name: names[i] || `选手 ${i + 1}`, family: MATCH_ART?.roster[names[i]] === "doubao" ? "mfuns-doubao" : MATCH_ART?.roster[names[i]] || Art.familyOf(names[i]),
                                                     color: Art.color(colors[i]) || Art.seatColor(i)}));
    const feed = [...byTick.values()].flat().filter(e => e.type === "fall").sort((a, b) => a.tick - b.tick);
    return {replay, header, frames, pos, frameOf, byTick, seats, feed, held: heldSpans(header, frames, pos, frameOf, byTick, seats.length),
            end: pos.length - 1, D: header.decision_ticks, rate: header.tick_rate};
  }
  /** For each seat, the [from, to) ticks it spends caught: reeled in by a hook (the replay has no state for that, only
      the event and the constant pull speed) or in the hitstun after being grabbed. */
  function heldSpans(header, frames, pos, frameOf, byTick, seats) {
    const hook = header.rules?.moves?.hook || {}, pull = hook.pull_speed || 0.9, limit = hook.pull_ticks || 30;
    const held = Array.from({length: seats}, () => []);
    for (const e of [...byTick.values()].flat()) {
      if (e.type === "hook_pull") {
        let end = e.tick + 2;
        while (end < e.tick + limit) {
          const body = pos[end + 1]?.p[e.target];
          if (!body || Math.hypot(body[2], body[3]) < pull * 0.85) break;
          end++;
        }
        held[e.target].push([e.tick, end + 1]);
      } else if (e.type === "hit" && e.skill === "grab") {
        const frame = frames[frameOf[Math.min(frameOf.length - 1, e.tick)]];
        held[e.target].push([e.tick, e.tick + frame.players[e.target].hitstun]);
      }
    }
    return held;
  }
  const SCENE = ["stage/bg_sky", "stage/bg_far", "stage/island", "stage/platform", "fx/wave", "fx/hook", "fx/hit_small", "fx/hit_big", "fx/ko"];
  // The scene and the standing figures go first: they fill the first picture, and the browser takes these requests in order.
  function preload() {
    const standing = R.seats.map(seat => Art.figurePath(seat.family, null));
    sprites.preload([...new Set([...SCENE.map(Art.scenePath), ...standing, ...R.seats.flatMap(seat => Art.allFigurePaths(seat.family))])]);
  }
  function open(replay) {
    R = index(replay);
    preload();
    for (const id of ["top", "side", "feed", "controls"]) $(id).hidden = false;
    $("picker").hidden = true;
    $("scrub").max = R.frames.length - 1;
    view.T = 0; view.feed = -1; view.card = "";
    const hash = new URLSearchParams(location.hash.slice(1));
    if (hash.has("t")) view.T = Math.min(R.end, Number(hash.get("t")) * R.D);
    if (hash.has("speed")) { view.speed = Number(hash.get("speed")) || 1; $("speed").value = String(view.speed); }
    if (hash.get("ui") === "0") $("controls").hidden = true;
    view.free = false;
    if (hash.has("z")) { view.free = true; view.cam.s = Number(hash.get("z")); const c = (hash.get("c") || "0,5").split(",").map(Number); view.cam.x = c[0]; view.cam.y = c[1]; }
    else view.cam = target();
    setPlaying(hash.get("play") === "1");
    board(); dirty = true;
  }
  function stateAt(T) { return R.frames[R.frameOf[Math.min(R.end, Math.ceil(T))]]; }
  /** Where every fighter is at the moment T: between the two ticks around it, or null when it is off the field. */
  function placed(T) {
    const t = Math.min(R.end, Math.floor(T)), a = R.pos[t], b = R.pos[Math.min(R.end, t + 1)], f = T - t;
    return a.p.map((p, i) => {
      const q = b.p[i];
      if (!p) return null;
      return q ? {x: p[0] + (q[0] - p[0]) * f, y: p[1] + (q[1] - p[1]) * f, vx: p[2], vy: p[3], facing: p[4]}
               : {x: p[0], y: p[1], vx: p[2], vy: p[3], facing: p[4]};
    });
  }

  // ---- camera ----
  function viewport() {
    const dpr = window.devicePixelRatio || 1, w = canvas.clientWidth, h = canvas.clientHeight;
    return {dpr, w, h};
  }
  function resize() {
    const {dpr, w, h} = viewport();
    canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr);
    if (R && !view.free) view.cam = target();
    dirty = true;
  }
  const PANELS = 620, CHROME = 170;                        // the width the side panels take, the height of the clock and controls
  function usable() { const {w, h} = viewport(); return {w: w > 1000 ? w - Math.min(PANELS, Math.round(w * 0.28)) : w, h: Math.max(240, h - CHROME)}; }
  function fit() { const u = usable(); return Math.min(u.w / 40, u.h / 24); }
  /** Keeps every fighter still in play in the picture, with the stage's middle as the anchor. */
  function target() {
    const {w, h} = usable(), base = fit();
    const people = placed(view.T).filter(p => p && Math.abs(p.x) < 30 && p.y > -18 && p.y < 26);
    let x0 = -16, x1 = 16, y0 = -5, y1 = 12;
    for (const p of people) { x0 = Math.min(x0, p.x - 3); x1 = Math.max(x1, p.x + 3); y0 = Math.min(y0, p.y - 3); y1 = Math.max(y1, p.y + 5); }
    const s = Math.max(base, Math.min(base * 1.5, Math.min(w / (x1 - x0), h / (y1 - y0))));
    return {x: (x0 + x1) / 2, y: (y0 + y1) / 2, s};
  }
  const sx = x => viewport().w / 2 + (x - view.cam.x) * view.cam.s;
  const sy = y => viewport().h / 2 - (y - view.cam.y) * view.cam.s;

  // ---- drawing ----
  function rounded(x, y, w, h, r) { ctx.beginPath(); ctx.roundRect(x, y, w, h, r); }
  function outlined(fill, line = 3) { ctx.fillStyle = fill; ctx.fill(); ctx.lineWidth = line; ctx.strokeStyle = INK; ctx.stroke(); }
  const scene = name => sprites.get(Art.scenePath(name));
  /** Draws a full-screen layer a little larger than the window, sliding with the camera by `drift` without showing an edge. */
  function layer(image, drift) {
    const {w, h} = viewport(), k = Math.max(w / image.naturalWidth, h / image.naturalHeight) * 1.12;
    const dw = image.naturalWidth * k, dh = image.naturalHeight * k, room = (dw - w) / 2;
    const shift = Math.max(-room, Math.min(room, -view.cam.x * view.cam.s * drift));
    ctx.drawImage(image, (w - dw) / 2 + shift, (h - dh) / 2, dw, dh);
  }
  function backdrop() {
    const {w, h} = viewport(), picture = scene("stage/bg_sky");
    if (picture) {
      layer(picture, 0.03);
      const far = scene("stage/bg_far");
      if (far) { ctx.globalAlpha = 0.75; layer(far, 0.12); ctx.globalAlpha = 1; }   // softened so the fighters stay the focus
      return;
    }
    const sky = ctx.createLinearGradient(0, 0, 0, h);
    sky.addColorStop(0, "#74c8ff"); sky.addColorStop(0.7, "#d9f3ff"); sky.addColorStop(1, "#fff1d6");
    ctx.fillStyle = sky; ctx.fillRect(0, 0, w, h);
    ctx.fillStyle = "rgba(255,255,255,.75)";
    for (const [cx, cy, r] of [[-20, 15, 2.2], [-11, 19, 1.7], [9, 17, 2.5], [21, 13, 1.9], [-25, 7, 1.5], [26, 21, 1.6]]) {
      const x = sx(cx * 1 + view.cam.x * 0.6), y = sy(cy + view.cam.y * 0.4), s = r * view.cam.s;
      ctx.beginPath(); ctx.arc(x, y, s, 0, 7); ctx.arc(x + s * 1.2, y + s * 0.2, s * 0.8, 0, 7); ctx.arc(x - s * 1.2, y + s * 0.25, s * 0.7, 0, 7); ctx.fill();
    }
    ctx.fillStyle = "#b6e8c0";
    ctx.beginPath(); ctx.moveTo(0, h);
    for (let i = 0; i <= 12; i++) { const x = (i / 12) * w; ctx.lineTo(x, h * 0.86 - Math.sin(i * 1.3 + view.cam.x * 0.03) * h * 0.04); }
    ctx.lineTo(w, h); ctx.fill();
  }
  function stage() {
    const s = view.cam.s, st = R.header.stage, island = scene("stage/island"), deck = scene("stage/platform");
    for (const b of st.solids) {
      const x = sx(b.x1), y = sy(b.top), w = (b.x2 - b.x1) * s, h = (b.top - b.bottom) * s;
      if (island) { const k = w / island.naturalWidth; ctx.drawImage(island, x, y - ISLAND_TOP * k, w, island.naturalHeight * k); continue; }
      rounded(x, y, w, h, 0.9 * s); outlined("#c98f5f", 4);
      rounded(x, y, w, 0.75 * s, [0.9 * s, 0.9 * s, 0.25 * s, 0.25 * s]); outlined("#7ed957", 4);
      ctx.fillStyle = "rgba(43,35,64,.14)";
      for (let i = 1; i < 7; i++) { ctx.beginPath(); ctx.arc(x + (w * i) / 7, y + h * (0.55 + 0.18 * Math.sin(i * 2.1)), 0.32 * s, 0, 7); ctx.fill(); }
    }
    for (const p of st.platforms) {
      if (deck) { const w = (p.x2 - p.x1) * s, k = w / deck.naturalWidth; ctx.drawImage(deck, sx(p.x1), sy(p.y) - DECK_TOP * k, w, deck.naturalHeight * k); continue; }
      rounded(sx(p.x1), sy(p.y), (p.x2 - p.x1) * s, 0.55 * s, 0.27 * s);
      outlined("#ffd23f", 3);
    }
  }
  // The pictures for each move, best first: a family without the newer poses falls back to the older one.
  /** The blast lines closing in late in a match: red outside them, and a warning as they start to move. */
  function danger(T) {
    const b = R.header.stage.blast, k = R.header.stage.blast_shrink;
    if (!k || T <= k.start_tick) return;
    const closed = (T - k.start_tick) * k.rate, half = Math.max(k.min_x, b.x - closed), top = Math.max(k.min_top, b.top - closed * k.top_ratio);
    const {w, h} = viewport(), left = sx(-half), right = sx(half), roof = sy(top);
    ctx.save(); ctx.fillStyle = "rgba(255,60,75,.3)";
    ctx.fillRect(0, 0, Math.max(0, left), h);
    ctx.fillRect(Math.min(w, right), 0, w, h);
    ctx.fillRect(Math.max(0, left), 0, Math.min(w, right) - Math.max(0, left), Math.max(0, roof));
    ctx.strokeStyle = "#ff3b4d"; ctx.lineWidth = 4; ctx.setLineDash([14, 8]);
    ctx.beginPath(); ctx.moveTo(left, h); ctx.lineTo(left, roof); ctx.lineTo(right, roof); ctx.lineTo(right, h); ctx.stroke();
    ctx.restore();
    if (T - k.start_tick < 90) caption(w / 2, 120, "边界开始收缩！", "#ff5d6c", 30);
  }
  const POSE = {uppercut: ["uppercut", "attack"], slam: ["slam_sit", "slam", "attack"], wave: ["wave", "throw"], hook: ["hook", "throw"],
                grab: ["grab", "attack"], blink: ["roll"], guard: ["guard"], counter: ["counter"]};
  const OUT_SWUNG = phase => phase === "active" || phase === "recover";
  function imageFor(seat, st, T, p) {
    const f = seat.family;
    if (!f) return null;
    const pose = name => sprites.get(Art.ACTION_POSES.includes(name) ? Art.actionPath(f, name) : Art.figurePath(f, name));
    const first = names => names.map(pose).find(Boolean);
    let picture = null;
    if (struggling(seat, T)) picture = first(["struggle_a", "hurt"]);
    else if (st.hitstun > 0) picture = pose(Math.hypot(p.vx, p.vy) > 0.5 ? "tumble" : "hurt");
    else if (st.act === "attack") picture = first(OUT_SWUNG(st.phase) ? ["strike_hit", "jab"] : ["strike_windup", "jab"]);
    else if (st.act === "charge") picture = first(OUT_SWUNG(st.phase) ? ["power_swing", "hammer_swing"] : ["power_charge", "hammer_charge"]);
    else if (POSE[st.act]) picture = first(POSE[st.act]);
    else if (!st.grounded) picture = pose(p.vy > 0 ? "jump" : "fall");
    else if (Math.abs(p.vx) > 0.05) picture = pose("walk_a");
    return picture || sprites.get(Art.figurePath(f, null));
  }
  // ---- body motion: one picture per move, moved and squashed here. All of it is a function of the moment T. ----
  // Per move and phase: lean (radians, + is forward), shift forward, lift, width and height factors.
  const BODY = {
    attack:   {windup: [-0.14, -0.1, 0, 1, 0.97], active: [0.18, 0.3, 0, 1.04, 1], recover: [0.06, 0.08, 0, 1, 1]},
    charge:   {windup: [-0.24, -0.18, 0, 0.98, 0.96], active: [0.26, 0.5, 0, 1.08, 1], recover: [0.1, 0.2, 0, 1, 1]},
    uppercut: {windup: [-0.05, 0, 0, 1.08, 0.9], active: [0.1, 0.1, 0.15, 0.9, 1.14], recover: [0, 0, 0, 1, 1]},
    slam:     {windup: [-0.1, 0, 0.5, 0.96, 1.06], active: [0, 0, 0, 0.9, 1.1], recover: [0, 0, 0, 1, 1]},
    grab:     {windup: [-0.1, -0.1, 0, 1, 0.98], active: [0.16, 0.36, 0, 1.06, 1], recover: [0.05, 0.1, 0, 1, 1]},
    wave:     {windup: [-0.12, -0.1, 0, 1, 0.98], recover: [-0.08, -0.08, 0, 1, 1]},
    hook:     {windup: [-0.12, -0.1, 0, 1, 0.98], recover: [-0.08, -0.08, 0, 1, 1]},
    counter:  {windup: [-0.06, 0, 0, 1, 0.97], recover: [0.03, 0, 0, 1, 1]},
    guard:    {active: [-0.04, -0.04, 0, 1.03, 0.96]},
  };
  // The show-only weapons (no numbers depend on them): how far the body throws itself and which trail the swing leaves.
  const SPEAR = {trail: "thrust", swing: 1.2};
  const OPUS_SABER = {trail: "saber", swing: 1.25};
  const WEAPONS = {claude: OPUS_SABER, fable: OPUS_SABER, gemini: {trail: "paw", swing: 1}, deepseek: {trail: "bowl", swing: 0.8},
                   sol: SPEAR, astra: SPEAR, terra: SPEAR, glm: {trail: "saber", swing: 1.1}, grok: {trail: "saber", swing: 0.8},
                   kimi: {trail: "thrust", swing: 1.3}, minimax: {trail: "thrust", swing: 1}, qwen: {trail: "fan", swing: 1},
                   doubao: {trail: "bowl", swing: 1.2}};
  const weaponOf = seat => WEAPONS[Art.spriteFamily(seat.family)];
  const PIVOT = 1.4, SQUASH_TICKS = 8, SLAM_RECOVER = 16;
  function struggling(seat, T) { return R.held[seat.i].some(([from, to]) => T >= from && T < to); }
  /** Ticks since this seat touched down, or null when it is in the air or landed long ago. */
  function landedAgo(seat, T) {
    const k = R.frameOf[Math.min(R.end, Math.ceil(T))];
    if (!R.frames[k].players[seat.i].grounded) return null;
    for (let j = k; j > Math.max(0, k - 3); j--)
      if (!R.frames[j - 1].players[seat.i].grounded) return Math.max(0, T - (j - 1) * R.D);
    return null;
  }
  function motion(seat, st, T, p) {
    const m = {rot: 0, dx: 0, dy: 0, w: 1, h: 1}, wave = rate => Math.sin(T * rate), speed = Math.hypot(p.vx, p.vy);
    if (st.act === "blink") return m;
    if (struggling(seat, T)) {
      m.rot = wave(1.9) * 0.24; m.dx = wave(2.7) * 0.1; m.dy = Math.abs(wave(2.2)) * 0.12;
      m.w = 1 + wave(3.1) * 0.05; m.h = 1 - wave(3.1) * 0.05;
      return m;
    }
    if (st.hitstun > 0) {
      if (speed > 0.5) m.rot = T * 0.45;                   // sent flying: end over end
      else m.dx = wave(3.7) * 0.1 * Math.min(1, st.hitstun / 8);
      return m;
    }
    if (st.act === "charge" && st.phase === "charging") {
      const k = Math.min(1, st.charge / 45);
      m.rot = -0.1 * k; m.dx = wave(5) * (0.02 + 0.07 * k); m.w = 1 + 0.05 * k; m.h = 1 - 0.07 * k;
      return m;
    }
    if (st.act === "slam" && st.phase === "recover" && st.grounded) {     // sat down hard
      const k = Math.min(1, st.phase_ticks / SLAM_RECOVER);
      m.w = 1 + 0.22 * k; m.h = 1 - 0.24 * k;
      return m;
    }
    const pose = BODY[st.act]?.[st.phase];
    if (pose) {
      const k = st.phase === "recover" ? Math.min(1, st.phase_ticks / 6) : 1;   // eases back to standing
      const swing = st.act === "attack" || st.act === "charge" ? weaponOf(seat)?.swing || 1 : 1;
      m.rot = pose[0] * k * swing; m.dx = pose[1] * k * swing; m.dy = pose[2] * k; m.w = 1 + (pose[3] - 1) * k; m.h = 1 + (pose[4] - 1) * k;
      return m;
    }
    if (st.act) return m;
    if (!st.grounded) {
      const k = Math.min(1, Math.abs(p.vy) / 0.8);
      m.w = 1 - 0.08 * k; m.h = 1 + 0.1 * k; m.rot = Math.max(-0.2, Math.min(0.2, p.vx * (p.facing || 1) * 0.5));
      return m;
    }
    if (Math.abs(p.vx) > 0.05) { m.dy = Math.abs(wave(0.9)) * 0.12; m.rot = 0.06 + wave(0.9) * 0.04; }
    else { m.w = 1 - wave(0.16) * 0.015; m.h = 1 + wave(0.16) * 0.015; }
    const age = landedAgo(seat, T), squash = age === null ? 0 : Math.max(0, 1 - age / SQUASH_TICKS);
    m.w *= 1 + 0.16 * squash; m.h *= 1 - 0.18 * squash;
    return m;
  }
  const feet = new Map();
  /** Where a picture's feet are: {left, right} in its pixels and `gap`, the empty rows under them. Null when the pixels
      cannot be read (a page opened from file://). */
  function footBox(image) {
    if (feet.has(image.dataset.path)) return feet.get(image.dataset.path);
    let box = null;
    try {
      const w = image.naturalWidth, h = image.naturalHeight, c = document.createElement("canvas");
      c.width = w; c.height = h;
      const g = c.getContext("2d", {willReadFrequently: true});
      g.drawImage(image, 0, 0);
      const data = g.getImageData(0, 0, w, h).data, solid = (x, y) => data[(y * w + x) * 4 + 3] > 40;
      let bottom = h - 1;
      for (; bottom > 0; bottom--) { let any = false; for (let x = 0; x < w && !any; x++) any = solid(x, bottom); if (any) break; }
      let left = w, right = -1;
      for (let y = Math.max(0, bottom - 16); y <= bottom; y++) for (let x = 0; x < w; x++) if (solid(x, y)) { left = Math.min(left, x); right = Math.max(right, x); }
      if (right >= 0) box = {left, right, gap: h - 1 - bottom};
    } catch (_) { /* tainted canvas: keep the picture where it is */ }
    feet.set(image.dataset.path, box);
    return box;
  }
  /** How far right of the canvas middle a weapon pose has its feet (the long weapons push the body off centre). */
  function footShift(image) {
    const box = /\/(strike|power)_[a-z]+_map/.test(image.dataset.path) ? footBox(image) : null;
    return box ? (box.left + box.right) / 2 - image.naturalWidth / 2 : 0;
  }
  const STRIDE_ROWS = 30;        // how far up from the ground a walking figure's legs are swapped
  /** The picture with the band of feet mirrored in place: the second step of a walk, the body and its facing unchanged. */
  function drawStride(image, w, h, k) {
    const box = footBox(image), W = image.naturalWidth, H = image.naturalHeight;
    if (!box) { ctx.drawImage(image, -w / 2, -h, w, h); return; }
    const L = Math.max(0, box.left - 3), R = Math.min(W, box.right + 4), top = H - box.gap - STRIDE_ROWS;
    ctx.drawImage(image, 0, 0, W, top, -w / 2, -h, w, top * k);
    const strip = (from, to) => { if (to > from) ctx.drawImage(image, from, top, to - from, H - top, -w / 2 + from * k, -h + top * k, (to - from) * k, (H - top) * k); };
    strip(0, L); strip(R, W);
    ctx.save(); ctx.translate(-w / 2 + ((L + R) / 2) * k, 0); ctx.scale(-1, 1);
    ctx.drawImage(image, L, top, R - L, H - top, (-(R - L) / 2) * k, -h + top * k, (R - L) * k, (H - top) * k);
    ctx.restore();
  }
  function figure(seat, st, T, p, alpha = 1) {
    const s = view.cam.s, x = sx(p.x), y = sy(p.y), image = imageFor(seat, st, T, p), facing = p.facing || 1, m = motion(seat, st, T, p);
    ctx.save(); ctx.globalAlpha = alpha; ctx.translate(x, y); ctx.scale(facing, 1);
    ctx.translate(m.dx * s, -m.dy * s - PIVOT * s); ctx.rotate(m.rot); ctx.translate(0, PIVOT * s); ctx.scale(m.w, m.h);
    if (image) {
      const k = ((FIGURE_HEIGHT * s) / 128) * Art.poseScale(image.dataset.path), w = image.naturalWidth * k, h = image.naturalHeight * k;
      const stepping = /\/walk_a_map/.test(image.dataset.path) && Math.floor(T / 5) % 2 === 1;
      if (/\/struggle_a_map/.test(image.dataset.path) && Math.floor(T / 3) % 2) ctx.scale(-1, 1);          // flailing from side to side
      const seated = /\/slam_sit_map/.test(image.dataset.path) && st.phase === "recover" && st.grounded;    // the picture leaves room under the body
      ctx.translate(-footShift(image) * k, seated ? (footBox(image)?.gap || 0) * k : 0);
      if (stepping) drawStride(image, w, h, k); else ctx.drawImage(image, -w / 2, -h, w, h);
    } else {
      ctx.beginPath(); ctx.arc(0, -0.95 * s, 0.95 * s, 0, 7); outlined(seat.color, 3);
      ctx.fillStyle = "#fff"; ctx.beginPath(); ctx.arc(0.35 * s, -1.15 * s, 0.22 * s, 0, 7); ctx.fill();
      ctx.fillStyle = INK; ctx.beginPath(); ctx.arc(0.42 * s, -1.15 * s, 0.1 * s, 0, 7); ctx.fill();
    }
    ctx.restore();
  }
  function ring(x, y, r, color, width) { ctx.beginPath(); ctx.arc(x, y, r, 0, 7); ctx.strokeStyle = color; ctx.lineWidth = width; ctx.stroke(); }
  function fighter(seat, st, T, p) {
    const s = view.cam.s, x = sx(p.x), y = sy(p.y), mid = y - 1.3 * s;
    if (st.invuln > 0 && Math.floor(T / 3) % 2) { nameTag(seat, st, x, y); return; }
    if (st.act === "blink" && st.phase === "active")
      for (let k = 4; k > 0; k--) { const old = placed(Math.max(0, T - k * 1.5))[seat.i]; if (old) figure(seat, st, T, old, 0.12 * (5 - k)); }
    if (st.act === "charge") {
      const glow = 1 + Math.min(1, st.charge / 45);
      ctx.save(); ctx.globalAlpha = 0.28 + 0.12 * Math.sin(T / 2); ctx.fillStyle = "#ff9a1f";
      ctx.beginPath(); ctx.arc(x, mid, glow * 1.3 * s, 0, 7); ctx.fill(); ctx.restore();
    }
    figure(seat, st, T, p);
    if (st.act === "slam" && st.phase === "recover" && st.grounded) dust(st, x, y);
    if (st.act === "guard") { ctx.save(); ctx.globalAlpha = 0.35; ctx.fillStyle = "#5ec8ff"; ctx.beginPath(); ctx.arc(x, mid, 1.75 * s, 0, 7); ctx.fill(); ctx.restore(); ring(x, mid, 1.75 * s, "#2f9be0", 3); }
    if (st.act === "counter" && st.phase === "active") ring(x, mid, (1.6 + 0.15 * Math.sin(T)) * s, "#ffc21a", 5);
    if (st.phase === "active" && ["attack", "charge", "uppercut", "slam", "grab"].includes(st.act)) {
      const trail = st.act === "attack" || st.act === "charge" ? weaponOf(seat)?.trail : null;
      if (trail) weaponTrail(trail, st, x, mid); else swoosh(st, x, mid);
    }
    nameTag(seat, st, x, y);
  }
  /** The dust of a slam landing, as wide as the move's real hit area. */
  function dust(st, x, y) {
    const s = view.cam.s, k = Math.min(1, st.phase_ticks / SLAM_RECOVER), reach = (R.header.rules?.moves?.slam?.radius || 1.1) * s * (1.25 - 0.25 * k);
    ctx.save(); ctx.globalAlpha = 0.85 * k; ctx.fillStyle = "#f4ecd8";
    for (const side of [-1, 1]) for (const [at, up, size] of [[0.5, 0.1, 0.34], [0.8, 0.28, 0.28], [1, 0.08, 0.22]]) {
      ctx.beginPath(); ctx.arc(x + side * at * reach, y - up * s, size * s * (0.7 + 0.5 * k), 0, 7); ctx.fill();
    }
    ctx.restore();
  }
  function weaponTrail(trail, st, x, y) {
    const s = view.cam.s, big = st.act === "charge" ? 1.35 : 1, [dx, dy] = st.dir, a = Math.atan2(-dy, dx);
    ctx.save(); ctx.translate(x, y); ctx.rotate(a); ctx.lineCap = "round";
    if (trail === "saber") {
      ctx.strokeStyle = "#e4f5ff"; ctx.lineWidth = 0.22 * big * s;
      ctx.beginPath(); ctx.arc(0, 0, 2.5 * big * s, -1.05, 1.05); ctx.stroke();
      ctx.strokeStyle = "#7fc8ff"; ctx.lineWidth = 0.07 * big * s;
      ctx.beginPath(); ctx.arc(0, 0, 2.72 * big * s, -1.05, 1.05); ctx.stroke();
    } else if (trail === "thrust") {
      ctx.strokeStyle = "#e4f5ff"; ctx.lineWidth = 0.2 * big * s;
      ctx.beginPath(); ctx.moveTo(0.9 * s, 0); ctx.lineTo(3.1 * big * s, 0); ctx.stroke();
      ctx.strokeStyle = "#7fc8ff"; ctx.lineWidth = 0.07 * big * s;
      for (const off of [-0.3, 0.3]) { ctx.beginPath(); ctx.moveTo(1.3 * s, off * big * s); ctx.lineTo(2.6 * big * s, off * big * s); ctx.stroke(); }
    } else if (trail === "fan") {
      ctx.fillStyle = "rgba(190,215,255,.55)";
      ctx.beginPath(); ctx.moveTo(0.6 * s, 0); ctx.arc(0.6 * s, 0, 2.3 * big * s, -0.75, 0.75); ctx.closePath(); ctx.fill();
    } else if (trail === "paw") {
      ctx.strokeStyle = "#ff8fc6"; ctx.lineWidth = 0.2 * big * s;
      for (const off of [-0.55, 0, 0.55]) { ctx.beginPath(); ctx.moveTo(1.2 * s, (off - 0.55) * big * s); ctx.quadraticCurveTo(2.5 * big * s, off * big * s, 1.6 * s, (off + 0.75) * big * s); ctx.stroke(); }
    } else {
      ctx.strokeStyle = "#dfe7ef"; ctx.lineWidth = 0.14 * big * s;
      ctx.beginPath(); ctx.arc(2 * s, 0, 0.75 * big * s, 0, 7); ctx.stroke();
      for (let i = 0; i < 8; i++) { const r = (i * Math.PI) / 4; ctx.beginPath(); ctx.moveTo(2 * s + Math.cos(r) * 0.95 * big * s, Math.sin(r) * 0.95 * big * s); ctx.lineTo(2 * s + Math.cos(r) * 1.45 * big * s, Math.sin(r) * 1.45 * big * s); ctx.stroke(); }
    }
    ctx.restore();
  }
  function swoosh(st, x, y) {
    const s = view.cam.s, [dx, dy] = st.dir, reach = (st.act === "charge" ? 2.7 : 2.1) * s, a = Math.atan2(-dy, dx);
    ctx.save(); ctx.strokeStyle = st.act === "grab" ? "#b18cff" : "#fff6d6"; ctx.lineWidth = (st.act === "charge" ? 0.75 : 0.45) * s; ctx.lineCap = "round";
    ctx.beginPath(); ctx.arc(x, y, reach, a - 0.8, a + 0.8); ctx.stroke();
    ctx.strokeStyle = INK; ctx.lineWidth = 2; ctx.beginPath(); ctx.arc(x, y, reach + ctx.lineWidth * 0 + 0.3 * s, a - 0.8, a + 0.8); ctx.stroke(); ctx.restore();
  }
  function nameTag(seat, st, x, y) {
    const s = view.cam.s, text = seat.name, top = y - 3.55 * s;
    ctx.font = "700 13px " + getComputedStyle(document.body).fontFamily;
    const w = ctx.measureText(text).width + 14;
    rounded(x - w / 2, top - 18, w, 20, 8); outlined(seat.color, 2);
    ctx.fillStyle = INK; ctx.textAlign = "center"; ctx.textBaseline = "middle"; ctx.fillText(text, x, top - 8);
    const bar = Math.max(w, 44);
    rounded(x - bar / 2, top + 4, bar, 6, 3); outlined("#efe6d6", 1.5);
    rounded(x - bar / 2, top + 4, bar * Math.max(0, st.meter), 6, 3); ctx.fillStyle = "#ff6fae"; ctx.fill();
  }
  function shots(T) {
    const t = Math.min(R.end, Math.floor(T)), s = view.cam.s, people = placed(T), st = stateAt(T);
    for (const [id, kind, x, y] of R.pos[t].s) {
      const px = sx(x), py = sy(y), owner = st.shots.find(b => b.id === id)?.owner;
      const before = R.pos[t - 1]?.s.find(b => b[0] === id), art = scene(kind === "hook" ? "fx/hook" : "fx/wave");
      const angle = before ? Math.atan2(before[3] - y, x - before[2]) : 0;   // screen y grows downward
      const spin = size => { ctx.save(); ctx.translate(px, py); ctx.rotate(angle); ctx.drawImage(art, -size / 2, -size / 2, size, size); ctx.restore(); };
      if (kind === "hook") {
        const o = people[owner];
        if (o) { ctx.beginPath(); ctx.moveTo(sx(o.x), sy(o.y + 1)); ctx.lineTo(px, py); ctx.strokeStyle = INK; ctx.lineWidth = 3; ctx.stroke(); }
        if (art) { spin(1.3 * s); continue; }
        ctx.beginPath(); ctx.arc(px, py, 0.4 * s, 0, 7); outlined("#c7cbe0", 3);
      } else if (art) {
        spin(2.2 * s);
      } else {
        ctx.save(); ctx.globalAlpha = 0.35; ctx.fillStyle = "#5ee8ff"; ctx.beginPath(); ctx.arc(px, py, 0.95 * s, 0, 7); ctx.fill(); ctx.restore();
        ctx.beginPath(); ctx.arc(px, py, 0.5 * s, 0, 7); outlined("#2fe0f5", 3);
      }
    }
  }
  function burst(x, y, size, color, points = 10) {
    ctx.beginPath();
    for (let i = 0; i < points * 2; i++) {
      const r = i % 2 ? size * 0.45 : size, a = (Math.PI * i) / points;
      ctx[i ? "lineTo" : "moveTo"](x + Math.cos(a) * r, y + Math.sin(a) * r);
    }
    ctx.closePath(); outlined(color, 3);
  }
  /** A burst picture when there is one, otherwise the hand-drawn star. */
  function flash(name, x, y, size, color, points) {
    const art = scene(name), d = 2.4 * size;
    if (art) ctx.drawImage(art, x - d / 2, y - d / 2, d, d);
    else burst(x, y, size, color, points);
  }
  function caption(x, y, text, color, size) {
    ctx.font = `800 ${size}px ` + getComputedStyle(document.body).fontFamily;
    ctx.textAlign = "center"; ctx.textBaseline = "middle"; ctx.lineJoin = "round";
    ctx.lineWidth = 5; ctx.strokeStyle = INK; ctx.strokeText(text, x, y);
    ctx.fillStyle = color; ctx.fillText(text, x, y);
  }
  function effects(T) {
    const s = view.cam.s, people = placed(T), {w, h} = viewport(), st = stateAt(T);
    for (let tick = Math.max(1, Math.floor(T) - 30); tick <= Math.floor(T); tick++) {
      for (const e of R.byTick.get(tick) || []) {
        const age = T - tick;
        if (e.type === "hit" && age < 14) {
          const p = R.pos[tick].p[e.target]; if (!p) continue;
          const k = 1 - age / 14, x = sx(p[0]), y = sy(p[1] + 1);
          ctx.save(); ctx.globalAlpha = Math.min(1, k * 1.6);
          flash(e.speed > 0.8 ? "fx/hit_big" : "fx/hit_small", x, y, (0.45 + Math.min(1.5, e.speed) * 0.5) * s * (0.6 + 0.4 * k), e.speed > 0.8 ? "#ff4d5e" : "#ffd23f");
          caption(x, y - (2.2 + (1 - k)) * s, `+${Math.round(e.damage)}`, "#fff", 18);
          ctx.restore();
        } else if (e.type === "block" && age < 10) {
          const p = R.pos[tick].p[e.seat]; if (p) ring(sx(p[0]), sy(p[1] + 1.3), (1.8 + age * 0.12) * s, "rgba(47,155,224,.8)", 4);
        } else if (e.type === "counter" && age < 18) {
          const p = R.pos[tick].p[e.seat]; if (!p) continue;
          ctx.save(); ctx.globalAlpha = 1 - age / 18; ring(sx(p[0]), sy(p[1] + 1.3), (1.8 + age * 0.3) * s, "#ffc21a", 7); caption(sx(p[0]), sy(p[1] + 4.6), "反击！", "#ffc21a", 24); ctx.restore();
        } else if (e.type === "hook_pull" && st.players[e.target].hitstun >= 0 && age < 20) {
          const a = people[e.seat], b = people[e.target];
          if (a && b) { ctx.beginPath(); ctx.moveTo(sx(a.x), sy(a.y + 1)); ctx.lineTo(sx(b.x), sy(b.y + 1)); ctx.strokeStyle = INK; ctx.lineWidth = 3; ctx.stroke(); }
        } else if (e.type === "fall" && age < 30) {
          const p = R.pos[tick - 1]?.p[e.seat]; if (!p) continue;
          const k = 1 - age / 30, side = w > 1000 ? PANELS / 2 + 70 : 70, x = Math.max(side, Math.min(w - side, sx(p[0]))), y = Math.max(120, Math.min(h - 150, sy(p[1])));
          ctx.save(); ctx.globalAlpha = k; flash("fx/ko", x, y, (2.6 + (1 - k) * 1.5) * s, R.seats[e.seat].color, 14);
          caption(x, y, e.kind === "kill" ? "击飞！" : "掉落", "#fff", 34); ctx.restore();
        }
      }
    }
  }
  function draw() {
    const {dpr} = viewport(), T = view.T, st = stateAt(T), people = placed(T);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    backdrop(); stage(); danger(T);
    shots(T);
    st.players.forEach((state, i) => { if (!state.out && state.respawn === 0 && people[i]) fighter(R.seats[i], state, T, people[i]); });
    effects(T);
  }

  // ---- panels ----
  function shade(damage) { const k = Math.min(1, damage / 150); return `rgb(${Math.round(43 + 212 * k)},${Math.round(35 + 42 * (1 - k))},${Math.round(64 + 30 * (1 - k))})`; }
  function portrait(seat) {
    const path = Art.imageUrl(Art.tokenPath(seat.family));
    return path ? `<img src="${esc(path)}" alt="">` : `<span class="disc" style="background:${seat.color}">${esc([...seat.name][0] || "?")}</span>`;
  }
  function clock(ticks) { const left = Math.max(0, Math.ceil(ticks / R.rate)); return `${Math.floor(left / 60)}:${String(left % 60).padStart(2, "0")}`; }
  function board() {
    const T = view.T, frame = stateAt(T), turn = R.frameOf[Math.min(R.end, Math.ceil(T))];
    $("turn").textContent = clock(R.header.max_turns * R.D - T);
    $("alive").textContent = frame.players.filter(p => !p.out).length;
    $("scrub").value = turn;
    const order = R.seats.slice().sort((a, b) => {
      const A = frame.players[a.i], B = frame.players[b.i];
      return (A.out - B.out) || (B.stocks - A.stocks) || (A.damage - B.damage) || (a.i - b.i);
    });
    const card = order.map(seat => {
      const p = frame.players[seat.i], stocks = "♥".repeat(p.stocks);
      return `<div class="fighter${p.out ? " out" : ""}">${portrait(seat)}<div class="who">${esc(seat.name)}</div>
        <div class="pct" style="color:${shade(p.damage)}">${Math.floor(p.damage)}<small>%</small></div><div class="stocks">${p.out ? "出局" : stocks}</div>
        <div class="bar"><i style="width:${Math.round(p.meter * 100)}%"></i></div></div>`;
    }).join("");
    if (card !== view.card) { view.card = card; $("board").innerHTML = card; }
    const shown = R.feed.filter(e => e.tick <= T).length;
    if (shown !== view.feed) {
      view.feed = shown;
      $("feed-list").innerHTML = R.feed.slice(0, shown).reverse().slice(0, 12).map(e => {
        const who = R.seats[e.seat].name;
        return `<li>${e.by === null ? `${esc(who)} 自己掉了下去` : `<b>${esc(R.seats[e.by].name)}</b> 击飞 ${esc(who)}`}</li>`;
      }).join("");
    }
    const over = T >= R.end && R.replay.result?.rank;
    $("banner").hidden = !over;
    if (over) {
      const first = R.replay.result.rank.indexOf(1);
      $("banner").textContent = `${R.seats[first].name} 获胜`;
    }
  }

  // ---- playing ----
  function setPlaying(on) { view.playing = on; view.last = performance.now(); $("play").textContent = on ? "⏸" : "▶"; }
  function seek(T) { view.T = Math.max(0, Math.min(R.end, T)); if (!view.free) view.cam = target(); board(); dirty = true; }
  function loop(now) {
    requestAnimationFrame(loop);
    if (R && view.playing) {
      view.T += ((now - view.last) / 1000) * R.rate * view.speed;
      if (view.T >= R.end) { view.T = R.end; setPlaying(false); }
      if (!view.free) {
        const goal = target(), k = 0.08;
        view.cam = {x: view.cam.x + (goal.x - view.cam.x) * k, y: view.cam.y + (goal.y - view.cam.y) * k, s: view.cam.s + (goal.s - view.cam.s) * k};
      }
      board(); dirty = true;
    }
    view.last = now;
    if (R && dirty) { dirty = false; draw(); }
  }
  $("play").onclick = () => { if (view.T >= R.end) view.T = 0; setPlaying(!view.playing); };
  $("first").onclick = () => seek(0);
  $("last").onclick = () => seek(R.end);
  $("prev").onclick = () => seek(view.T - R.D);
  $("next").onclick = () => seek(view.T + R.D);
  $("speed").onchange = event => { view.speed = Number(event.target.value); };
  $("scrub").oninput = event => seek(Number(event.target.value) * R.D);
  $("fit").onclick = () => { view.free = false; seek(view.T); };
  $("file").onchange = event => { if (event.target.files[0]) readFile(event.target.files[0]); };
  addEventListener("dragover", event => event.preventDefault());
  addEventListener("drop", event => { event.preventDefault(); if (event.dataTransfer.files[0]) readFile(event.dataTransfer.files[0]); });
  addEventListener("resize", resize);
  addEventListener("keydown", event => {
    if (!R || event.target.tagName === "INPUT" && event.target.type !== "range") return;
    if (event.code === "Space") { event.preventDefault(); $("play").click(); }
    else if (event.code === "ArrowLeft") seek(view.T - R.D);
    else if (event.code === "ArrowRight") seek(view.T + R.D);
  });
  window.__debug = {seek, state: () => ({T: view.T, cam: view.cam, end: R && R.end, held: R && R.held})};
  resize();
  requestAnimationFrame(loop);
  if (REPLAY) open(REPLAY);
  else picker();
})();
