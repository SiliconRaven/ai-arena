/* Which pictures a seat gets (assets/CREDITS.md): an entrant's model family picks its avatar token and its map figure
   (the little character on the arena); an entrant no family is recognised in (a baseline, an unknown model) gets a disc
   in the seat's colour with the name's first letter. The seat colour still tells the seats apart. Plain script: it
   puts `DungeonArt` on the window (in node: module.exports). */
(function (root) {
  "use strict";
  const FAMILIES = ["claude", "chatgpt", "sol", "astra", "terra", "gemini", "deepseek", "glm", "grok", "kimi", "qwen", "minimax", "hy", "fable"];
  const LABELS = {claude: "Claude", chatgpt: "ChatGPT", gemini: "Gemini", deepseek: "DeepSeek", glm: "GLM",
                  grok: "Grok", kimi: "Kimi", qwen: "Qwen", minimax: "MiniMax", sol: "GPT Sol", astra: "GPT Astra", terra: "GPT Terra",
                  hy: "混元 HY", fable: "Claude Fable"};
  const PORTRAITS = {"mfuns-deepseek": ["deepseek", "DeepSeek 立绘"], "mfuns-chatgpt": ["chatgpt", "ChatGPT 立绘"],
                     "mfuns-claude": ["claude", "Claude 立绘"], "mfuns-kimi": ["kimi", "Kimi 立绘"],
                     "mfuns-qwen": ["qwen", "千问 立绘"], "mfuns-gemini": ["gemini", "Gemini 立绘"],
                     "mfuns-glm": ["glm", "GLM 立绘"],
                     "mfuns-grok": ["grok", "Grok 立绘"], "mfuns-minimax": ["minimax", "MiniMax 立绘"],
                     "mfuns-doubao": [null, "豆包"]};
  const AVATARS = [...FAMILIES, ...Object.keys(PORTRAITS)];
  /** Names that are drawn as another character (user 2026-11-08: "astra就是chatgpt"). */
  const SAME_CHARACTER = {chatgpt: "astra"};
  /** Keywords in a seat's name or entrant id, first match wins. */
  const KEYWORDS = [["claude", "claude"], ["anthropic", "claude"], ["chatgpt", "chatgpt"], ["openai", "chatgpt"],
                    ["codex", "chatgpt"], ["gpt", "chatgpt"], ["gemini", "gemini"], ["deepseek", "deepseek"],
                    ["zhipu", "glm"], ["glm", "glm"], ["grok", "grok"], ["kimi", "kimi"], ["moonshot", "kimi"],
                    ["qwen", "qwen"], ["minimax", "minimax"]];
  const HY_MODEL = /hunyuan|混元|(?:^|\/)hy(?=$|[\d\s._-])/;
  const FABLE_MODEL = /(?:^|[-./_\s])fable(?=$|[-./_\s\d])/;
  /** Seat colours (seat 0 first), picked to read on the arena floor and the abyss and to differ from each other. */
  const SEAT_COLORS = ["#ff8a3d", "#2fe0f5", "#9dff7a", "#ff4fc0", "#f8c844", "#a98bff", "#ff5d6c", "#4f8bff",
                       "#e8e2ff", "#3dd6a0"];

  /** The model family named in any of `texts` (a seat's name, entrant id …), or null. */
  function familyOf(...texts) {
    for (const text of texts) {
      if (!text) continue;
      const lower = String(text).trim().toLowerCase();
      if (FABLE_MODEL.test(lower)) return "fable";
      if (HY_MODEL.test(lower)) return "hy";
      for (const variant of ["sol", "astra", "terra"]) {
        if (lower === variant || new RegExp(`(?:gpt|codex|openai|chatgpt)[\\w.-]*[-_.]${variant}(?:$|[-_.])`).test(lower)) return variant;
      }
      for (const [key, family] of KEYWORDS) if (lower.includes(key)) return family;
    }
    return null;
  }

  function seatColor(i) { return SEAT_COLORS[i % SEAT_COLORS.length]; }
  function color(value) { return typeof value === "string" && /^#[0-9a-f]{6}$/i.test(value) ? value.toLowerCase() : null; }
  function label(family) { return PORTRAITS[family]?.[1] || LABELS[family] || "中性（基准／未知）"; }

  /** Portrait choices reuse their family's figure set. */
  function spriteFamily(avatar) {
    const set = FAMILIES.includes(avatar) ? avatar : PORTRAITS[avatar]?.[0] || null;
    return SAME_CHARACTER[set] || set;
  }

  function tokenPath(family) {
    if (Object.hasOwn(PORTRAITS, family)) { const name = family.slice(6); return `assets/avatars/${SAME_CHARACTER[name] || name}.png`; }
    return FAMILIES.includes(family) ? `assets/avatars/${SAME_CHARACTER[family] || family}.png` : null;
  }
  const FIGURE_POSES = [null, "walk_a", "walk_b", "attack", "repair", "hit", "destroyed"];
  const SETS_WITHOUT_LEGACY_POSES = new Set(["hy", "fable"]);
  const CURRENT_POSES = new Set(["walk_a", "walk_b", "jump", "fall", "dash_charge", "guard", "counter", "uppercut", "slam_sit",
                                 "grab", "throw", "wave", "hook", "hurt", "tumble", "roll", "struggle_a", "struggle_b",
                                 "strike_windup", "strike_hit", "power_charge", "power_swing"]);
  /** Includes null for the standing picture. Return a copy so callers cannot change the contract. */
  function figurePoses() { return FIGURE_POSES.slice(); }
  /** Standing characters are 128 px; action canvases are 192 px with idle anchored at (32,64). */
  function figurePath(family, pose) {
    const set = family === "mfuns-doubao" ? "doubao" : spriteFamily(family);
    if (!set) return null;
    if (!pose || !FIGURE_POSES.includes(pose)) return `assets/figures/${set}.png`;
    if (SETS_WITHOUT_LEGACY_POSES.has(set) && !CURRENT_POSES.has(pose)) return null;
    return `assets/figures/${set}/${pose}_map_192.png`;
  }
  /** Action poses drawn for the knockback game (`<pose>_map_192.png`, facing right, the same 192 px canvas and foot
      line as the other poses). A family may not have them yet: the caller falls back when the picture fails to load. */
  const ACTION_POSES = ["dash_charge", "roll", "hammer_charge", "hammer_swing", "jab", "throw", "hurt", "tumble",
                        "jump", "fall", "guard", "counter", "uppercut", "slam", "grab", "wave", "hook",
                        "strike_windup", "strike_hit", "power_charge", "power_swing", "slam_sit", "struggle_a", "struggle_b"];
  function actionPath(family, pose) {
    if (!ACTION_POSES.includes(pose)) return null;
    const set = family === "mfuns-doubao" ? "doubao" : spriteFamily(family);
    if (SETS_WITHOUT_LEGACY_POSES.has(set) && !CURRENT_POSES.has(pose)) return null;
    return set ? `assets/figures/${set}/${pose}_map_192.png` : null;
  }
  function scenePath(name) { return `assets/${name}.png`; }
  /** Every picture a seat of this family can be drawn with: the standing figure, the old poses and the action poses
      (for preloading when a replay opens). */
  function allFigurePaths(family) {
    const paths = new Set([...FIGURE_POSES.map(pose => figurePath(family, pose)), ...ACTION_POSES.map(pose => actionPath(family, pose))]);
    paths.delete(null);
    return [...paths];
  }
  // Written by scripts/pose_scales.py: how much a pose is drawn up or down so the body stays the size of the standing figure.
  /* pose-scales:start */ const POSE_SCALES = {"claude":{"walk_a":0.929,"walk_b":0.929,"power_swing":1.114},"sol":{"walk_a":0.95,"walk_b":0.942,"guard":0.926,"counter":0.897,"grab":0.934,"wave":0.904,"hook":0.911,"hurt":0.934,"strike_windup":0.95,"strike_hit":1.097,"power_charge":0.934},"astra":{"walk_a":0.956,"walk_b":0.916,"hurt":0.956,"strike_windup":1.112,"strike_hit":1.079,"power_swing":1.172},"terra":{"walk_a":0.903,"walk_b":0.911,"guard":0.887,"counter":0.903,"grab":0.903,"wave":0.895,"hook":0.887,"hurt":0.864,"power_charge":0.903},"gemini":{"guard":1.059,"counter":1.049,"grab":1.059,"wave":1.059,"hook":1.059,"strike_windup":1.069,"strike_hit":1.059,"power_charge":1.059,"power_swing":1.069},"deepseek":{"walk_a":0.924,"walk_b":0.924,"guard":0.948,"counter":0.932,"grab":0.94,"wave":0.932,"hook":0.94,"hurt":0.924,"strike_windup":0.924,"strike_hit":0.932,"power_charge":0.932,"power_swing":0.916},"glm":{"walk_a":0.933,"walk_b":0.941,"counter":1.167,"grab":1.2,"wave":0.933,"hook":0.933,"hurt":1.057,"strike_windup":0.868,"strike_hit":0.911,"power_charge":0.941},"grok":{"walk_a":0.939,"walk_b":0.947,"grab":0.947,"wave":0.947,"hook":0.939,"hurt":0.939,"strike_windup":0.956,"strike_hit":1.049,"power_charge":0.947},"kimi":{"walk_a":0.921,"walk_b":0.928,"guard":0.921,"counter":0.928,"grab":0.928,"wave":0.921,"hook":0.921,"hurt":0.928,"strike_windup":0.928,"power_charge":0.928,"power_swing":1.094},"qwen":{"guard":0.905,"counter":0.905,"grab":0.905,"wave":0.905,"hook":0.905,"strike_windup":0.912,"strike_hit":0.898,"power_charge":0.905,"power_swing":0.958},"minimax":{"guard":0.921,"counter":0.921,"grab":0.928,"wave":0.921,"hook":0.921,"strike_windup":0.921,"strike_hit":0.921,"power_charge":0.921,"power_swing":0.913},"doubao":{"guard":0.929,"counter":0.921,"grab":0.921,"wave":0.921,"hook":0.929,"strike_windup":0.921,"strike_hit":0.929,"power_charge":0.929,"power_swing":0.929}}; /* pose-scales:end */
  function poseScale(path) {
    const found = /figures\/(\w+)\/(\w+)_map_192\.png/.exec(path || "");
    return (found && POSE_SCALES[found[1]]?.[found[2]]) || 1;
  }
  function isPose(path) { return typeof path === "string" && /_map_(96|192)\.png$/.test(path); }

  function imageUrl(path) {
    const encoded = path && MATCH_ART?.images[path.slice("assets/".length)];
    return encoded ? "data:image/png;base64," + encoded : null;
  }

  /** Loads pictures once; `get` returns the image when loaded (else null and `onload` fires later), `grey` a greyed
      copy (canvas filter; drawing a file:// image this way does not need to read its pixels). */
  function Sprites(onload) {
    const images = new Map(), greys = new Map();
    function get(path) {
      if (!path) return null;
      let entry = images.get(path);
      if (!entry) {
        const url = imageUrl(path);
        if (!url) return null;
        const image = new Image();
        image.dataset.path = path;
        entry = {image, ready: false};
        image.onload = () => { entry.ready = true; onload(); };
        image.onerror = () => { entry.failed = true; };
        image.src = url;
        images.set(path, entry);
      }
      return entry.ready ? entry.image : null;
    }
    function grey(path) {
      const image = get(path);
      if (!image) return null;
      let canvas = greys.get(path);
      if (!canvas) {
        canvas = document.createElement("canvas");
        canvas.width = image.naturalWidth;
        canvas.height = image.naturalHeight;
        const ctx = canvas.getContext("2d");
        ctx.filter = "grayscale(1) brightness(1.3)";
        ctx.drawImage(image, 0, 0);
        greys.set(path, canvas);
      }
      return canvas;
    }
    /** Starts loading `paths` and resolves when each has loaded or failed. */
    function preload(paths) {
      return Promise.all(paths.filter(Boolean).map(path => {
        get(path);
        const entry = images.get(path);
        if (!entry) return null;
        const {image} = entry;
        // Cached images may be complete before their load callback runs. Wait for our actual ready/failed state.
        if (entry.ready || entry.failed) return null;
        return new Promise(done => {
          image.addEventListener("load", done, {once: true});
          image.addEventListener("error", done, {once: true});
        });
      }));
    }
    return {get, grey, preload};
  }

  const api = {FAMILIES, AVATARS, spriteFamily, familyOf, seatColor, color, label, tokenPath, figurePath, figurePoses, actionPath, scenePath, poseScale, ACTION_POSES, allFigurePaths, isPose, imageUrl, Sprites};
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.DungeonArt = api;
})(typeof globalThis !== "undefined" ? globalThis : this);
