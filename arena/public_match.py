"""Run local matches with the public roster and retain their replays."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import uuid

from arena import native_match, replay

ROOT = Path(__file__).resolve().parent.parent
RULES = json.loads((ROOT / 'game/rules.json').read_text(encoding='utf-8'))


def roster():
    competitors = json.loads((ROOT / 'entrants/roster.json').read_text(encoding='utf-8'))
    practice = json.loads((ROOT / 'entrants/practice-roster.json').read_text(encoding='utf-8'))
    return competitors + practice


def select(bot, ids):
    folder = Path(bot).expanduser().resolve()
    if not (folder / 'bot.cpp').is_file():
        raise ValueError('你的程序文件夹中必须有 bot.cpp')
    if len(ids) != len(set(ids)):
        raise ValueError('不能重复选择同一个对手')
    if not RULES['min_players'] <= len(ids) + 1 <= RULES['max_players']:
        raise ValueError(f"请选择 {RULES['min_players'] - 1}–{RULES['max_players'] - 1} 位对手")
    entries = {entry['id']: entry for entry in roster()}
    if any(key not in entries for key in ids):
        raise ValueError('对手不在名单中，请刷新页面')
    bots = [('你', folder)]
    for key in ids:
        entry = entries[key]
        path = ROOT / 'entrants' / key
        if not (path / 'bot.cpp').is_file():
            raise ValueError(f"找不到 {entry['name']} 的 bot.cpp")
        bots.append((entry['name'], path))
    if len({name for name, _ in bots}) != len(bots):
        raise ValueError('参赛名称必须互不相同')
    return bots


def play(bots, games, seed, progress=lambda _: None, on_match=lambda _: None):
    if type(games) is not int or not 1 <= games <= 100000:
        raise ValueError('场数必须是 1–100000 的整数')
    if type(seed) is not int or seed < 0 or seed + games - 1 > 2**31 - 1:
        raise ValueError('种子及最后一局种子须在 0–2147483647 之间')
    output = ROOT / 'output' / uuid.uuid4().hex
    output.mkdir(parents=True)
    totals = [{'name': name, 'score': 0, 'wins': 0, 'kills': 0} for name, _ in bots]
    matches = []
    def state(status, completed):
        return {'status': status, 'games': games, 'completed': completed,
                'totals': [dict(total) for total in totals], 'output': output.name}

    progress(state('running', 0))
    prepared = native_match.prepare(bots, RULES, ROOT / 'output/cache')
    for i in range(games):
        result = native_match.run_prepared(prepared, seed + i, RULES)
        values = result['result']
        for seat, total in enumerate(totals):
            total['score'] += values['score'][seat]
            total['kills'] += values['kills'][seat]
            total['wins'] += values['rank'][seat] == 1
        filename = f'{i + 1:04}.html'
        (output / filename).write_text(replay.render_html(result), encoding='utf-8')
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
    parser.add_argument('--bot', required=True, help='你的 C++ 程序文件夹')
    parser.add_argument('--opponents', nargs='+', required=True, help='对手 ID，见 entrants/roster.json')
    parser.add_argument('--games', type=int, default=2)
    parser.add_argument('--seed', type=int, default=1)
    args = parser.parse_args()
    try:
        bots = select(args.bot, args.opponents)
        result = play(bots, args.games, args.seed,
                      lambda state: print(f"\r已完成 {state['completed']}/{state['games']}", end='', flush=True))
        print('\n' + json.dumps(result, ensure_ascii=False, indent=2))
    except (ValueError, OSError, native_match.CompileError) as exc:
        parser.exit(1, f'\n出错：{exc}\n')


if __name__ == '__main__':
    main()
