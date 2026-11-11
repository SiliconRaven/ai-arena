"""Compile and run trusted C++ programs with the local referee."""
from __future__ import annotations
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import uuid

from arena import KNOCKBACK_ROOT

COMPILER_FLAGS = ('-std=c++17', '-O2', '-fno-fast-math', '-ffp-contract=off')


class CompileError(ValueError):
    def __init__(self, output, seat=None):
        self.output, self.seat = output, seat
        super().__init__(output)


if os.name == 'nt':
    # Match the mathematical constants exposed by the Linux compiler.
    COMPILER_FLAGS += ('-D_USE_MATH_DEFINES',)


def compiler():
    found = os.environ.get('CXX') or shutil.which('g++') or shutil.which('c++')
    if not found:
        raise SystemExit('no C++ compiler found (g++); set CXX or report the missing compiler to the organizer')
    return found


# The referee's numeric flags must match native/Makefile (CXXFLAGS without warnings, FPFLAGS).
REFEREE_FLAGS = ('-O2', '-std=c++17', '-fno-fast-math', '-ffp-contract=off')
TRUSTED_EXTRA = {'step_referee': ('-pthread',), 'step_supervisor': ('-pthread',), 'step_boot': ()}
if os.name == 'nt':
    TRUSTED_EXTRA = {name: (*flags, '-lshell32') for name, flags in TRUSTED_EXTRA.items()}
EXE = '.exe' if os.name == 'nt' else ''


def build_trusted(cache: Path, native: Path = KNOCKBACK_ROOT / 'native') -> dict:
    """The referee, supervisor and launcher, compiled from native/ into the build cache (`referee-<hash>/`, by
    sources, compiler and flags): native/ itself may be read-only (the contestant package)."""
    digest = hashlib.sha256(json.dumps([compiler(), REFEREE_FLAGS, TRUSTED_EXTRA]).encode())
    for path in sorted([*native.glob('*.cpp'), *(native / 'include').glob('*.hpp')]):
        digest.update(path.name.encode() + b'\0' + path.read_bytes() + b'\0')
    out = Path(cache).resolve() / f'referee-{digest.hexdigest()[:16]}'
    out.mkdir(parents=True, exist_ok=True)
    for name, extra in TRUSTED_EXTRA.items():
        target = out / (name + EXE)
        if target.is_file():
            continue
        partial = out / f'.{name}.{uuid.uuid4().hex}.part{EXE}'
        built = subprocess.run([compiler(), *REFEREE_FLAGS, str(native / f'{name}.cpp'), *extra, '-o', str(partial)],
                               capture_output=True, text=True, errors='replace')
        if built.returncode:
            partial.unlink(missing_ok=True)
            raise SystemExit(f'building the native referee ({name}) failed:\n' + built.stdout + built.stderr)
        install(partial, target)
    return {name: out / (name + EXE) for name in TRUSTED_EXTRA}


def folder_digest(folder: Path) -> str:
    digest = hashlib.sha256(json.dumps([compiler(), COMPILER_FLAGS]).encode())
    for path in sorted(p for p in folder.rglob('*') if p.is_file()):
        digest.update(path.relative_to(folder).as_posix().encode() + b'\0' + path.read_bytes() + b'\0')
    return digest.hexdigest()


def install(partial: Path, target: Path) -> None:
    try:
        if not target.is_file():
            os.replace(partial, target)
    except PermissionError:
        if not target.is_file():
            raise
    finally:
        partial.unlink(missing_ok=True)


def build_program(folder: Path, cache: Path) -> Path:
    """Compile a program folder as an official submission would be, reusing an identical earlier build."""
    folder = folder.resolve(strict=True)
    sources = sorted(folder.glob('*.cpp'))
    if not (folder / 'bot.cpp').is_file():
        raise CompileError('a program folder needs bot.cpp')
    stage = os.name == 'nt' and not str(folder).isascii()
    if stage and (folder == cache.resolve() or folder in cache.resolve().parents):
        raise CompileError('use a build cache outside the program folder')
    out = cache / folder_digest(folder)[:16]
    binary = out / ('bot' + EXE)
    if binary.is_file():
        return binary
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='build-', dir=out) as temporary:
        partial = Path(temporary) / binary.name
        if stage:
            # MinGW needs ASCII paths for sources, even when Python can read the original folder.
            staged = Path(temporary) / 'source'
            shutil.copytree(folder, staged)
            folder = staged
        compiled = subprocess.run([compiler(), *COMPILER_FLAGS, *(str(folder / p.name) for p in sources), '-o', str(partial)],
                                  cwd=folder, capture_output=True, text=True, errors='replace', timeout=300)
        if compiled.returncode:
            raise CompileError((compiled.stdout + compiled.stderr)[-65536:])
        install(partial, binary)
    return binary


def windows_worker_limits(rules: dict) -> str:
    sandbox = rules['sandbox']
    size, unit = re.fullmatch(r'([1-9][0-9]*)([kKmMgG]?)', str(sandbox['memory'])).groups()
    memory = int(size) * {'': 1, 'k': 2**10, 'm': 2**20, 'g': 2**30}[unit.lower()]
    processes = int(sandbox['pids']) + 1  # Includes the launcher process.
    # Keep the upstream fourfold headroom for latency-sensitive decisions.
    cpu_rate = max(1, min(10000, round(40000 * float(sandbox['cpus']) / (os.cpu_count() or 1))))
    return f'{memory}:{processes}:{cpu_rate}'


def prepare(bots: list[tuple[str, Path]], rules: dict, cache: Path, *,
            native: Path = KNOCKBACK_ROOT / 'native', limit_workers: bool = False) -> dict:
    """Compile each seat once; subsequent matches use these same binaries."""
    cache = Path(cache).resolve()
    names = [name for name, _ in bots]
    if len(set(names)) != len(names):
        raise ValueError('bot names must be unique')
    if not int(rules['min_players']) <= len(names) <= int(rules['max_players']):
        raise ValueError(f"a match has {rules['min_players']}-{rules['max_players']} players, got {len(names)}")
    trusted = build_trusted(cache, native)
    binaries, built = [], {}
    for seat, (_, folder) in enumerate(bots):
        key = str(Path(folder).resolve())
        if key not in built:
            try:
                built[key] = build_program(Path(folder), cache)
            except CompileError as error:
                error.seat = seat
                raise
        binaries.append(built[key])
    supervisor = str(trusted['step_supervisor'])
    limits = ['--worker-limits', windows_worker_limits(rules)] if limit_workers and os.name == 'nt' else []
    commands = [[supervisor, *limits, '--trusted-local-gated', str(trusted['step_boot']), '--trusted-local', str(binary)]
                for binary in binaries]
    return {'referee': str(trusted['step_referee']), 'supervisor': supervisor,
            'commands': commands, 'names': names,
            'local_programs': [{'name': n, 'binary_sha256': hashlib.sha256(b.read_bytes()).hexdigest()}
                               for n, b in zip(names, binaries)]}


def run_prepared(prepared: dict, seed: int, rules: dict) -> dict:
    """Run one match from a previously prepared set of binaries."""
    started = datetime.datetime.now(datetime.timezone.utc).isoformat()
    request = {'supervisor': prepared['supervisor'], 'commands': prepared['commands'],
               'names': prepared['names'], 'seed': seed, 'rules': rules,
               'invalid': [None] * len(prepared['names'])}
    played = subprocess.run([prepared['referee'], '--experimental-supervised'], input=json.dumps(request) + '\n',
                            capture_output=True, text=True, encoding='utf-8')
    if played.returncode:
        raise SystemExit('the referee stopped:\n' + played.stderr[-4000:])
    replay = json.loads(played.stdout)
    replay['header']['local_programs'] = prepared['local_programs']
    replay['header'].update(started_utc=started, finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    return replay


def run(bots: list[tuple[str, Path]], seed: int, rules: dict, cache: Path) -> dict:
    """Compile the programs and run a single local match."""
    return run_prepared(prepare(bots, rules, cache), seed, rules)
