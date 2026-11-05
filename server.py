"""Loopback-only UI for running trusted local programs."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import threading
from urllib.parse import parse_qs, urlsplit
import uuid

from arena import public_match

ROOT = Path(__file__).resolve().parent
jobs = {}
match_lock = threading.Lock()
jobs_guard = threading.Lock()
PAGE_SIZE = 20


def job_page(job, page):
    matches = job['matches']
    pages = max(1, (len(matches) + PAGE_SIZE - 1) // PAGE_SIZE)
    page = max(1, min(page, pages))
    start = (page - 1) * PAGE_SIZE
    return {**{key: value for key, value in job.items() if key != 'matches'},
            'matches': matches[start:start + PAGE_SIZE], 'page': page, 'pages': pages}


class Handler(BaseHTTPRequestHandler):
    def allowed(self):
        host = self.headers.get('Host', '')
        expected = {f'127.0.0.1:{self.server.server_port}', f'localhost:{self.server.server_port}'}
        if host not in expected:
            return False
        origin = self.headers.get('Origin')
        return origin is None or origin == f'http://{host}'

    def respond(self, status, data, content_type='application/json; charset=utf-8'):
        if not isinstance(data, bytes):
            data = json.dumps(data, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if not self.allowed():
            return self.respond(403, {'error': '禁止跨站访问'})
        if self.path == '/':
            return self.respond(200, (ROOT / 'index.html').read_bytes(), 'text/html; charset=utf-8')
        if self.path == '/api/roster':
            return self.respond(200, {'roster': public_match.roster(), 'min': public_match.RULES['min_players'],
                                      'max': public_match.RULES['max_players']})
        url = urlsplit(self.path)
        if url.path.startswith('/assets/'):
            directory = ROOT / 'assets'
            image = (directory / url.path.removeprefix('/assets/')).resolve()
            if image.is_relative_to(directory) and image.suffix == '.png' and image.is_file():
                return self.respond(200, image.read_bytes(), 'image/png')
            return self.respond(404, {'error': '找不到角色图片'})
        if url.path.startswith('/api/jobs/'):
            try:
                page = int(parse_qs(url.query).get('page', ['1'])[0])
            except ValueError:
                return self.respond(400, {'error': '页码不正确'})
            with jobs_guard:
                job = jobs.get(url.path.removeprefix('/api/jobs/'))
                result = job_page(job, page) if job else None
            return self.respond(200 if result else 404, result or {'error': '任务不存在'})
        if self.path.startswith('/output/'):
            path = (ROOT / self.path.lstrip('/')).resolve()
            if path.is_relative_to(ROOT / 'output') and path.suffix == '.html' and path.is_file():
                return self.respond(200, path.read_bytes(), 'text/html; charset=utf-8')
        self.respond(404, {'error': '找不到页面'})

    def do_POST(self):
        if not self.allowed() or self.headers.get('Origin') != f"http://{self.headers.get('Host')}":
            return self.respond(403, {'error': '禁止跨站操作'})
        if self.path != '/api/jobs' or self.headers.get('Content-Type') != 'application/json':
            return self.respond(404, {'error': '找不到操作'})
        try:
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length <= 65536:
                raise ValueError('请求大小不正确')
            data = json.loads(self.rfile.read(length))
            bots = public_match.select(data['bot'], data['opponents'])
            games, seed = data['games'], data['seed']
            if type(games) is not int or not 1 <= games <= 100000 or type(seed) is not int or seed < 0 or seed + games - 1 > 2**31 - 1:
                raise ValueError('场数或种子不正确')
        except (KeyError, TypeError, ValueError, OSError) as exc:
            return self.respond(400, {'error': str(exc)})
        if not match_lock.acquire(blocking=False):
            return self.respond(409, {'error': '已有对战正在进行，请等待完成'})
        job_id = uuid.uuid4().hex
        jobs[job_id] = {'status': 'running', 'games': games, 'completed': 0, 'matches': [], 'totals': []}

        def run():
            def progress(state):
                with jobs_guard:
                    jobs[job_id].update(state)

            def on_match(match):
                with jobs_guard:
                    jobs[job_id]['matches'].append(match)

            try:
                public_match.play(bots, games, seed, progress, on_match)
            except (Exception, SystemExit) as exc:
                with jobs_guard:
                    jobs[job_id].update(status='error', error=str(exc))
            finally:
                match_lock.release()

        threading.Thread(target=run, daemon=True).start()
        self.respond(202, {'id': job_id})


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8786)
    args = parser.parse_args()
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    print(f'Open http://127.0.0.1:{server.server_port}', flush=True)
    server.serve_forever()
