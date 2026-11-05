"""Write a replay as one self-contained HTML file (the viewer in game/viewer.html with the replay embedded)."""
from __future__ import annotations

import base64
import json
import gzip
import re
from functools import lru_cache
from pathlib import Path

VIEWER = Path(__file__).resolve().parent.parent / "game" / "viewer.html"
PLACEHOLDER = "/*REPLAY*/null"
ROOT = VIEWER.parents[1]
CHARACTERS = {row["name"]: row["character"] for row in
              json.loads((ROOT / "entrants/roster.json").read_text(encoding="utf-8"))}


@lru_cache(maxsize=len(CHARACTERS))
def character_images(character: str) -> dict[str, str]:
    assets = ROOT / "assets"
    files = [assets / "avatars" / f"{character}.png", assets / "figures" / f"{character}.png",
             *sorted((assets / "figures" / character).glob("*_map_192.png"))]
    return {file.relative_to(assets).as_posix(): base64.b64encode(file.read_bytes()).decode("ascii")
            for file in files}


def read_replay(path: Path) -> dict:
    """Read JSON, gzip JSON or our standalone HTML without executing any of its JavaScript."""
    raw = path.read_bytes()
    text = (gzip.decompress(raw) if raw[:2] == b"\x1f\x8b" else raw).decode("utf-8-sig").strip()
    if text.startswith("<"):
        found = re.search(r"\bconst REPLAY=(\{[\s\S]*?\});[ \t]*\r?\n[ \t]*const COLORS\s*=", text)
        if not found:
            raise ValueError("HTML has no embedded knockback replay")
        text = found[1]
    replay = json.loads(text)
    if not isinstance(replay, dict) or not isinstance(replay.get("header"), dict) or not isinstance(replay.get("frames"), list):
        raise ValueError("not a knockback replay")
    return replay


def render_html(replay: dict, colors: list | None = None) -> str:
    """The viewer reads the header, frames and result; the records and debug output stay in the JSON beside it."""
    shown = {key: value for key, value in replay.items() if key not in ("records", "debug")}
    if colors is not None:
        shown["presentation"] = dict(shown.get("presentation") or {}) | {"colors": colors}
    data = json.dumps(shown, separators=(",", ":")).replace("</", "<\\/")
    page = VIEWER.read_text(encoding="utf-8")
    images = {}
    for name in set(shown["header"].get("names", [])):
        if name in CHARACTERS:
            images.update(character_images(CHARACTERS[name]))
    art = json.dumps({"roster": CHARACTERS, "images": images}, separators=(",", ":"))
    page = page.replace("/*MATCH_ART*/null", art, 1)
    page = page.replace("/*SCORING_SCRIPT*/", VIEWER.with_name("scoring.js").read_text(encoding="utf-8"))
    if PLACEHOLDER not in page:
        raise RuntimeError("viewer.html has no replay placeholder")
    if "presentation" in shown:
        marker = "const header = REPLAY.header"
        if marker not in page:
            raise RuntimeError("viewer.html has no palette insertion point")
        palette = '''for (const [i, color] of (REPLAY.presentation?.colors || []).entries()) {
  if (typeof color === "string" && /^#[0-9a-f]{6}$/i.test(color)) COLORS[i] = color;
}
'''
        page = page.replace(marker, palette + marker, 1)
    return page.replace(PLACEHOLDER, data, 1)


def write_html(replay: dict, path: Path) -> None:
    Path(path).write_text(render_html(replay), encoding="utf-8")
