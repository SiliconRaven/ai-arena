"""Build a standalone local player package from the public repository."""
from __future__ import annotations

import argparse
import io
import json
import os
from pathlib import Path
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
FILES = (
    "LICENSE", "RULES.md", "PROMPT.md", "start.cmd", "server.py", "index.html",
    "player-package/README.md",
    "arena/__init__.py", "arena/native_match.py", "arena/public_match.py",
    "arena/replay.py", "arena/package.py",
    "game/rules.json", "game/viewer.html", "game/scoring.js",
    "native/step_referee.cpp", "native/step_supervisor.cpp", "native/step_boot.cpp",
    "entrants/practice-roster.json",
)
PROGRAM_SUFFIXES = {".cpp", ".h", ".hpp", ".hh", ".inc"}


def contents() -> dict[str, bytes]:
    files = {name: (ROOT / name).read_bytes() for name in FILES}
    files["README.md"] = files["player-package/README.md"]
    files["entrants/roster.json"] = b"[]\n"
    for header in sorted((ROOT / "native/include").glob("*.hpp")):
        files[header.relative_to(ROOT).as_posix()] = header.read_bytes()
    practice = json.loads(files["entrants/practice-roster.json"])
    folders = [ROOT / "baseline", *(ROOT / "entrants" / row["id"] for row in practice)]
    for folder in folders:
        for source in sorted(folder.rglob("*")):
            if source.is_file() and source.suffix in PROGRAM_SUFFIXES:
                name = source.relative_to(ROOT).as_posix()
                files[name] = source.read_bytes()
                if name.startswith("baseline/"):
                    files["my-bot/" + name.removeprefix("baseline/")] = files[name]
    return files


def bundle() -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(contents().items()):
            entry = zipfile.ZipInfo("ai-arena-player/" + name, (2026, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, data)
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "output/player-package.zip")
    args = parser.parse_args()
    data = bundle()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=args.out.parent, suffix=".zip", delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    try:
        os.replace(temporary, args.out)
    finally:
        temporary.unlink(missing_ok=True)
    print(f"Player package: {args.out.resolve()}")


if __name__ == "__main__":
    main()
