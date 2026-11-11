"""Build a standalone local player package from the public repository."""
from __future__ import annotations

import argparse
import io
import os
from pathlib import Path
import tempfile
import zipfile

from arena import ROOT, public_match, replay

SHARED_FILES = (
    "LICENSE", "start.cmd", "server.py", "index.html",
    "arena/__init__.py", "arena/native_match.py", "arena/public_match.py",
    "arena/replay.py", "arena/package.py",
)
GAME_FILES = (
    "workspace/RULES.md", "prompts/PROMPT.md", "PLAYER_PACKAGE.md", "rules.json",
    "native/step_referee.cpp", "native/step_supervisor.cpp", "native/step_boot.cpp",
    "workspace/practice/roster.json",
)
PROGRAM_SUFFIXES = {".cpp", ".h", ".hpp", ".hh", ".inc"}


def contents(game=public_match.DEFAULT_GAME) -> dict[str, bytes]:
    prefix = game.root.relative_to(ROOT).as_posix()
    files = {name: (ROOT / name).read_bytes() for name in SHARED_FILES}
    files.update({f"{prefix}/{name}": (game.root / name).read_bytes() for name in GAME_FILES})
    files["README.md"] = files[f"{prefix}/PLAYER_PACKAGE.md"]
    files[f"{prefix}/entrants/roster.json"] = b"[]\n"
    assets = game.root / "assets"
    for path in [*(game.root / "native/include").glob("*.hpp"),
                 *(game.root / "replay").glob("*"),
                 *assets.glob("stage/*.png"), *assets.glob("fx/*.png"), *assets.glob("CREDITS.md")]:
        if path.is_file() and path.suffix in {".hpp", ".html", ".css", ".js", ".png", ".md"}:
            files[path.relative_to(ROOT).as_posix()] = path.read_bytes()
    for character in game.characters:
        for path in replay.character_files(assets, character):
            files[path.relative_to(ROOT).as_posix()] = path.read_bytes()
    baseline = game.root / "workspace/baseline"
    folders = [baseline, *(game.practice_root / row["id"] for row in game.practice())]
    for folder in folders:
        for source in sorted(folder.rglob("*")):
            if source.is_file() and source.suffix in PROGRAM_SUFFIXES:
                name = source.relative_to(ROOT).as_posix()
                files[name] = source.read_bytes()
                if folder == baseline:
                    files[f"{prefix}/workspace/my-bot/{source.relative_to(baseline).as_posix()}"] = files[name]
    return files


def bundle(game=public_match.DEFAULT_GAME) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(contents(game).items()):
            entry = zipfile.ZipInfo("ai-arena-player/" + name, (2026, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, data)
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", choices=public_match.GAMES, default=public_match.DEFAULT_GAME.id)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    name = "player-package.zip" if args.game == "knockback" else f"{args.game}-player-package.zip"
    out = args.out or ROOT / "output" / name
    data = bundle(public_match.GAMES[args.game])
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=out.parent, suffix=".zip", delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    try:
        os.replace(temporary, out)
    finally:
        temporary.unlink(missing_ok=True)
    print(f"Player package: {out.resolve()}")


if __name__ == "__main__":
    main()
