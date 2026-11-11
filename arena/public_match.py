"""Run local matches with the public roster and retain their replays."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from functools import cached_property
import json
from pathlib import Path
import uuid

from arena import GAMES_DIR, ROOT, native_match, replay

CHARACTER_NAMES = {
    'astra': 'GPT Astra', 'claude': 'Claude', 'deepseek': 'DeepSeek', 'doubao': '豆包',
    'fable': 'Claude Fable', 'gemini': 'Gemini', 'glm': 'GLM', 'grok': 'Grok', 'hy': '混元 HY',
    'kimi': 'Kimi', 'qwen': '千问', 'sol': 'GPT Sol', 'sonnet': 'Claude Sonnet', 'terra': 'GPT Terra',
}


@dataclass(frozen=True)
class Game:
    id: str
    title: str
    include_bot: bool
    limit_workers: bool = False

    @property
    def root(self):
        return GAMES_DIR / f'ai-{self.id}'

    @property
    def practice_root(self):
        return self.root / 'workspace/practice'

    @cached_property
    def rules(self):
        return json.loads((self.root / 'rules.json').read_text(encoding='utf-8'))

    @cached_property
    def characters(self):
        return {key: CHARACTER_NAMES.get(key, key) for key in replay.character_ids(self.root / 'assets')}

    def competitors(self):
        return json.loads((self.root / 'entrants/roster.json').read_text(encoding='utf-8'))

    def practice(self):
        return json.loads((self.practice_root / 'roster.json').read_text(encoding='utf-8'))

    def roster(self):
        return self.competitors() + self.practice()

    def options(self):
        competitors, practice = self.competitors(), self.practice()
        default_bot = (self.root / 'workspace/my-bot').relative_to(ROOT).as_posix()
        return {'id': self.id, 'title': self.title, 'default_bot': default_bot,
                'include_bot': self.include_bot or not competitors, 'roster': competitors + practice,
                'characters': self.characters,
                'practice_ids': [row['id'] for row in practice],
                'min': self.rules['min_players'], 'max': self.rules['max_players']}


KNOCKBACK = Game('knockback', '擂台乱斗', include_bot=True)
SMASH = Game('smash', 'AI Smash · 平台混战', include_bot=False, limit_workers=True)
GAMES = {game.id: game for game in (KNOCKBACK, SMASH) if (game.root / 'rules.json').is_file()}
DEFAULT_GAME = next(iter(GAMES.values()))
RULES = DEFAULT_GAME.rules


def roster():
    return DEFAULT_GAME.roster()


def select(bot, ids, *, game=DEFAULT_GAME):
    if not isinstance(ids, (list, tuple)) or any(not isinstance(key, str) for key in ids):
        raise ValueError('对手名单格式不正确')
    bots = []
    if bot is not None:
        folder = Path(bot).expanduser().resolve()
        if folder == ROOT / 'my-bot' and not folder.exists():
            folder = game.root / 'workspace/my-bot'
        if not (folder / 'bot.cpp').is_file():
            raise ValueError('你的程序文件夹中必须有 bot.cpp')
        bots.append(('你', folder))
    if len(ids) != len(set(ids)):
        raise ValueError('不能重复选择同一个对手')
    minimum, maximum = (game.rules[key] - len(bots) for key in ('min_players', 'max_players'))
    if not minimum <= len(ids) <= maximum:
        count = str(minimum) if minimum == maximum else f'{minimum}–{maximum}'
        raise ValueError(f'请选择 {count} 位内置选手')
    entries = {entry['id']: (entry['name'], game.root / 'entrants' / entry['id'])
               for entry in game.competitors()}
    entries.update({entry['id']: (entry['name'], game.practice_root / entry['id'])
                    for entry in game.practice()})
    if any(key not in entries for key in ids):
        raise ValueError('对手不在名单中，请刷新页面')
    for key in ids:
        name, path = entries[key]
        if not (path / 'bot.cpp').is_file():
            raise ValueError(f"找不到 {name} 的 bot.cpp")
        bots.append((name, path))
    if len({name for name, _ in bots}) != len(bots):
        raise ValueError('参赛名称必须互不相同')
    return bots


def play(bots, games, seed, progress=lambda _: None, on_match=lambda _: None, *, game=DEFAULT_GAME, character=None):
    if type(games) is not int or not 1 <= games <= 100000:
        raise ValueError('场数必须是 1–100000 的整数')
    if type(seed) is not int or seed < 0 or seed + games - 1 > 2**31 - 1:
        raise ValueError('种子及最后一局种子须在 0–2147483647 之间')
    output = ROOT / 'output' / uuid.uuid4().hex
    output.mkdir(parents=True)
    totals = [{'name': name, 'score': 0, 'wins': 0, 'kills': 0} for name, _ in bots]
    matches = []
    def state(status, completed):
        return {'status': status, 'game': game.id, 'game_title': game.title, 'games': games, 'completed': completed,
                'totals': [dict(total) for total in totals], 'output': output.name}

    progress(state('running', 0))
    cache = ROOT / 'output/cache' / game.id
    prepared = native_match.prepare(bots, game.rules, cache,
                                    native=game.root / 'native', limit_workers=game.limit_workers)
    for i in range(games):
        result = native_match.run_prepared(prepared, seed + i, game.rules)
        if character is not None:
            result.setdefault('presentation', {})['characters'] = {bots[0][0]: character}
        values = result['result']
        for seat, total in enumerate(totals):
            total['score'] += values['score'][seat]
            total['kills'] += values['kills'][seat]
            total['wins'] += values['rank'][seat] == 1
        filename = f'{i + 1:04}.html'
        (output / filename).write_text(replay.render_html(result, root=game.root), encoding='utf-8')
        (output / f'{i + 1:04}.json').write_text(json.dumps(result, ensure_ascii=False), encoding='utf-8')
        match = {'number': i + 1, 'seed': seed + i, 'replay': f'/output/{output.name}/{filename}',
                 'ranks': values['rank'], 'scores': values['score'], 'kills': values['kills']}
        matches.append(match)
        on_match(match)
        progress(state('running', i + 1))
    final = state('done', games)
    (output / 'summary.json').write_text(json.dumps({**final, 'matches': matches}, ensure_ascii=False, indent=2),
                                         encoding='utf-8')
    progress(final)
    return final


def main():
    parser = argparse.ArgumentParser(description='运行本地 C++ 对战')
    parser.add_argument('--game', choices=GAMES, default=DEFAULT_GAME.id, help='游戏 ID')
    parser.add_argument('--bot', help='你的 C++ 程序文件夹。省略则只使用内置选手')
    parser.add_argument('--opponents', nargs='+', required=True, help='所选游戏的内置选手 ID')
    parser.add_argument('--games', type=int, default=2)
    parser.add_argument('--seed', type=int, default=1)
    args = parser.parse_args()
    try:
        game = GAMES[args.game]
        bots = select(args.bot, args.opponents, game=game)
        result = play(bots, args.games, args.seed,
                      lambda state: print(f"\r已完成 {state['completed']}/{state['games']}", end='', flush=True),
                      game=game)
        print('\n' + json.dumps(result, ensure_ascii=False, indent=2))
    except (ValueError, OSError, native_match.CompileError) as exc:
        parser.exit(1, f'\n出错：{exc}\n')


if __name__ == '__main__':
    main()
