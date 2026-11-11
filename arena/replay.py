"""Embed replay data and assets in each game's standalone HTML viewer."""
from __future__ import annotations

import base64
import json
import gzip
import re
from functools import lru_cache
from pathlib import Path

from arena import KNOCKBACK_ROOT

PLACEHOLDER = "/*REPLAY*/null"


def character_ids(assets: Path) -> list[str]:
    return [figure.stem for figure in sorted((assets / "figures").glob("*.png"))
            if figure.is_file() and (assets / "avatars" / figure.name).is_file()]


def character_files(assets: Path, character: str) -> list[Path]:
    files = [assets / "avatars" / f"{character}.png", assets / "figures" / f"{character}.png",
             *sorted((assets / "figures" / character).glob("*_map_192.png"))]
    return [file for file in files if file.is_file()]


@lru_cache(maxsize=32)
def character_images(assets: Path, character: str) -> dict[str, str]:
    return {file.relative_to(assets).as_posix(): base64.b64encode(file.read_bytes()).decode("ascii")
            for file in character_files(assets, character)}


@lru_cache
def scene_images(assets: Path) -> dict[str, str]:
    return {file.relative_to(assets).as_posix(): base64.b64encode(file.read_bytes()).decode("ascii")
            for group in ("stage", "fx") for file in sorted((assets / group).glob("*.png"))}


@lru_cache
def viewer_page(root: Path) -> str:
    viewer = root / "replay/viewer.html"
    page = viewer.read_text(encoding="utf-8")
    page = re.sub(r'<link rel="stylesheet" href="([\w.-]+\.css)">',
                  lambda match: "<style>\n" + viewer.with_name(match[1]).read_text(encoding="utf-8") + "</style>",
                  page)

    def script(match):
        code = viewer.with_name(match[1]).read_text(encoding="utf-8").replace("</script", "<\\/script")
        return "<script>\n" + code + "</script>"

    page = re.sub(r'<script src="([\w.-]+\.js)"></script>', script, page)
    if "/*SCORING_SCRIPT*/" in page:
        page = page.replace("/*SCORING_SCRIPT*/", viewer.with_name("scoring.js").read_text(encoding="utf-8"))
    return page


def read_replay(path: Path) -> dict:
    """Read JSON, gzip JSON or our standalone HTML without executing any of its JavaScript."""
    raw = path.read_bytes()
    text = (gzip.decompress(raw) if raw[:2] == b"\x1f\x8b" else raw).decode("utf-8-sig").strip()
    if text.startswith("<"):
        found = re.search(r"\bconst REPLAY=(\{[\s\S]*?\});[ \t]*\r?\n[ \t]*const COLORS\s*=", text)
        if not found:
            raise ValueError("HTML has no embedded arena replay")
        text = found[1]
    replay = json.loads(text)
    if not isinstance(replay, dict) or not isinstance(replay.get("header"), dict) or not isinstance(replay.get("frames"), list):
        raise ValueError("not an arena replay")
    return replay


def render_html(replay: dict, colors: list | None = None, *, root: Path = KNOCKBACK_ROOT) -> str:
    """The viewer reads the header, frames and result; the records and debug output stay in the JSON beside it."""
    shown = {key: value for key, value in replay.items() if key not in ("records", "debug")}
    if colors is not None:
        shown["presentation"] = dict(shown.get("presentation") or {}) | {"colors": colors}
    data = json.dumps(shown, separators=(",", ":")).replace("</", "<\\/")
    page = viewer_page(root)
    characters = {row["name"]: row["character"] for row in
                  json.loads((root / "entrants/roster.json").read_text(encoding="utf-8")) if row.get("character")}
    assets = root / "assets"
    overrides = (shown.get("presentation") or {}).get("characters")
    if isinstance(overrides, dict):
        available = character_ids(assets)
        characters.update({name: character for name, character in overrides.items() if character in available})
    images = dict(scene_images(assets))
    for name in set(shown["header"].get("names", [])):
        if name in characters:
            images.update(character_images(assets, characters[name]))
    art = json.dumps({"roster": characters, "images": images}, separators=(",", ":")).replace("</", "<\\/")
    page = page.replace("/*MATCH_ART*/null", art, 1)
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
