#!/usr/bin/env python3
"""Mock Docker Unix HTTP server; exercise native cgroup/UID/tar/cache code."""
from contextlib import contextmanager
import http.server
import io
import json
import os
from pathlib import Path
import socketserver
import subprocess
import tarfile
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'tests' / 'build'
IDENTITY = 'a1' * 32
OTHER = 'b2' * 32


def archive(text, name='passwd', pax=False):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w', format=tarfile.PAX_FORMAT) as tar:
        entry = tarfile.TarInfo(name)
        if pax:
            entry.pax_headers['comment'] = 'test extended header'
        body = text.encode()
        entry.size = len(body)
        tar.addfile(entry, io.BytesIO(body))
    return stream.getvalue()


@contextmanager
def daemon(directory, containers=None, passwd=None, fail_list=False, fail_archive=False, slow=False):
    requests = []
    containers = containers if containers is not None else [{'Id': IDENTITY, 'Names': ['/palworld-server-lxh']}]
    passwd = passwd if passwd is not None else {IDENTITY: archive('root:x:0:0::/root:/bin/sh\nsteam:x:1000:1000::/home/steam:/bin/sh\n')}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append(('GET', self.path))
            if slow:
                time.sleep(1.5)
            if self.path == '/containers/json':
                body = json.dumps(containers).encode()
                code = 500 if fail_list else 200
            elif self.path.startswith('/containers/') and self.path.endswith('/archive?path=/etc/passwd'):
                body = passwd.get(self.path.split('/')[2], b'')
                code = 404 if fail_archive else 200
            else:
                body, code = b'', 400
            try:
                self.send_response(code)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, *args):
            pass

    class Server(socketserver.ThreadingUnixStreamServer):
        daemon_threads = True

    path = str(Path(directory) / 'docker.sock')
    server = Server(path, Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield path, requests
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


class DockerUsersTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        BUILD.mkdir(exist_ok=True)
        subprocess.run(['/usr/bin/gcc', '-std=c99', '-D_DEFAULT_SOURCE', '-I.', '-Ilinux', '-pthread',
                        'tests/DockerUsersProbe.c', 'linux/DockerUsers.c', 'XUtils.c',
                        '-lcurl', '-ljson-c', '-o', str(BUILD / 'docker-users-probe')], cwd=ROOT, check=True)

    def probe(self, socket, directory, uid=1000, group=None, mapping='0 0 4294967295\n', loops=20):
        path = Path(directory) / 'uid_map'
        if mapping is not None:
            path.write_text(mapping)
        result = subprocess.check_output([str(BUILD / 'docker-users-probe'), socket,
                     group if group is not None else f'/docker/{IDENTITY}', str(uid), str(path), str(loops)],
                     text=True, timeout=4)
        return result.splitlines()

    def test_container_username_and_read_only_requests(self):
        with tempfile.TemporaryDirectory() as directory, daemon(directory) as (socket, requests):
            labels = self.probe(socket, directory)
            self.assertEqual(labels[-1], '[palworld-server-lxh]steam')
            self.assertEqual(requests, [('GET', '/containers/json'),
                             ('GET', f'/containers/{IDENTITY}/archive?path=/etc/passwd')])

    def test_cgroup_v1_v2_systemd_and_nested_paths(self):
        for group in (f'0::/system.slice/docker-{IDENTITY}.scope',
                      f'/user.slice/user-1000.slice/app.slice/docker-{IDENTITY}.scope/child',
                      f'/docker/{IDENTITY}/child;/docker/{IDENTITY}',
                      f'2:cpu:/docker/{IDENTITY}\n0::/docker/{IDENTITY}\n'):
            with self.subTest(group=group), tempfile.TemporaryDirectory() as directory, daemon(directory) as (socket, _):
                self.assertEqual(self.probe(socket, directory, group=group)[-1], '[palworld-server-lxh]steam')

    def test_host_process_and_false_positive_ids(self):
        for group in ('/', '/user.slice/session-1.scope', f'/other/{IDENTITY}',
                      f'/docker/{IDENTITY}extra', '/docker/abcd', f'/docker-{IDENTITY}.scoped'):
            with self.subTest(group=group), tempfile.TemporaryDirectory() as directory:
                labels = self.probe(str(Path(directory) / 'absent.sock'), directory, group=group, loops=1)
                self.assertEqual(labels, ['HOST'])

    def test_user_namespace_uid_mapping(self):
        with tempfile.TemporaryDirectory() as directory, daemon(directory) as (socket, _):
            labels = self.probe(socket, directory, uid=101000, mapping='0 100000 65536\n')
            self.assertEqual(labels[-1], '[palworld-server-lxh]steam')

    def test_unknown_user_and_missing_uid_map_never_use_host_username(self):
        with tempfile.TemporaryDirectory() as directory, daemon(directory) as (socket, _):
            self.assertEqual(self.probe(socket, directory, uid=1234)[-1], '[palworld-server-lxh]1234')
        with tempfile.TemporaryDirectory() as directory, daemon(directory) as (socket, _):
            self.assertEqual(self.probe(socket, directory, mapping=None)[-1], '[palworld-server-lxh]uid:1000')

    def test_multiple_containers_with_same_uid(self):
        containers = [{'Id': IDENTITY, 'Names': ['/one']}, {'Id': OTHER, 'Names': ['/two']}]
        passwd = {IDENTITY: archive('alice:x:1000:1000::/:/bin/sh\n'),
                  OTHER: archive('bob:x:1000:1000::/:/bin/sh\n')}
        for identity, expected in ((IDENTITY, '[one]alice'), (OTHER, '[two]bob')):
            with self.subTest(identity=identity), tempfile.TemporaryDirectory() as directory:
                with daemon(directory, containers, passwd) as (socket, _):
                    self.assertEqual(self.probe(socket, directory, group=f'/docker/{identity}')[-1], expected)

    def test_failed_daemon_or_passwd(self):
        for fail_list, fail_archive, expected in ((True, False, f'[{IDENTITY[:12]}]1000'),
                                                 (False, True, '[palworld-server-lxh]1000')):
            with self.subTest(fail_list=fail_list), tempfile.TemporaryDirectory() as directory:
                with daemon(directory, fail_list=fail_list, fail_archive=fail_archive) as (socket, _):
                    self.assertEqual(self.probe(socket, directory)[-1], expected)
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(self.probe(str(Path(directory) / 'absent.sock'), directory)[-1], f'[{IDENTITY[:12]}]1000')

    def test_pax_tar_and_malformed_passwd(self):
        text = 'bad line\nnope:x:bad:0::/:/bin/sh\noverflow:x:9999999999999999999999:0::/:/bin/sh\nsteam:x:1000:1000::/:/bin/sh\n'
        with tempfile.TemporaryDirectory() as directory, daemon(directory, passwd={IDENTITY: archive(text, pax=True)}) as (socket, _):
            self.assertEqual(self.probe(socket, directory)[-1], '[palworld-server-lxh]steam')

    def test_slow_daemon_does_not_block_ui_or_exit(self):
        with tempfile.TemporaryDirectory() as directory, daemon(directory, slow=True) as (socket, _):
            start = time.monotonic()
            labels = self.probe(socket, directory, loops=5)
            self.assertEqual(len(labels), 5)
            self.assertLess(time.monotonic() - start, 1.2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
