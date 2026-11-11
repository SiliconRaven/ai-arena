/* Pure fair-share display arithmetic, shared by the console and standalone replays. */
(function(root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.DungeonScoring = api;
})(typeof globalThis !== "undefined" ? globalThis : this, function() {
  "use strict";
  let settings = {c: 1, rank_max: 100};
  function configure(value) {
    if (!["number", "string"].includes(typeof value.c) || String(value.c).trim() === "") throw new Error("Invalid scoring settings");
    const c = Number(value.c), rank_max = Number(value.rank_max ?? 100);
    if (!Number.isFinite(c) || c < 0 || !Number.isFinite(rank_max) || rank_max <= 0) throw new Error("Invalid scoring settings");
    const changed = c !== settings.c || rank_max !== settings.rank_max;
    settings = {c, rank_max};
    return changed;
  }
  function parts(n, rank, kills, options = settings) {
    if (!Number.isInteger(n) || n < 2 || !Number.isFinite(rank) || rank < 1 || rank > n || !Number.isFinite(kills)) return null;
    const placement = options.rank_max * (n - rank) / (n - 1);
    const bonus = options.c * kills * n / (n - 1);
    return {placement, bonus, total: placement + bonus, kills};
  }
  function resultParts(result, n, seat) {
    const kills = result.kill_points?.[seat] ?? (Number.isFinite(result.rank_points?.[seat]) && Number.isFinite(result.score?.[seat])
      ? result.score[seat] - result.rank_points[seat] : null);
    return parts(n, result.rank?.[seat], kills);
  }
  return {configure, parts, resultParts, settings: () => ({...settings})};
});
