"""Profile a real SQL reload and fresh offline DDL on the public sample.

Run only against an isolated stopped runtime directory made by local tests.
The existing GPU/CPU tables are retained. A new database receives a fresh graph.
"""
import argparse
import json
import os
import pathlib
import signal
import socket
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from mysqlclient_adapter import Connection


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['binary', 'bridge', 'base-dir', 'fixtures', 'output']:
        parser.add_argument('--' + name, type=pathlib.Path, required=True)
    parser.add_argument('--gpu-database', required=True)
    parser.add_argument('--cpu-database', required=True)
    parser.add_argument('--port', type=int, default=2881)
    parser.add_argument('--profiler', choices=['rocprofv3', 'none'], default='rocprofv3',
                        help='Explicitly select functional-only mode if profiler startup fails')
    args = parser.parse_args()
    if not (args.base_dir / 'run/seekdb.pid').is_file():
        parser.error('Require an existing isolated seekdb runtime with persisted indexes')
    with socket.socket() as sock:
        if sock.connect_ex(('127.0.0.1', args.port)) == 0:
            parser.error('Port is occupied; leave the existing service untouched')
    args.output.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, LD_LIBRARY_PATH=str(args.bridge.resolve().parent) +
                       ':/opt/hipvs/lib:/opt/rocm/lib')
    server = [str(args.binary.resolve()), '--base-dir=' + str(args.base_dir.resolve()),
              f'--port={args.port}', '--nodaemon', '--parameter', '_memory_budget=16G',
              '--parameter', 'datafile_size=8G', '--parameter', 'datafile_maxsize=12G',
              '--parameter', 'log_disk_size=4G', '--log-level', 'INFO']
    command = ['rocprofv3', '--kernel-trace', '--memory-copy-trace', '--stats',
               '--output-format', 'csv', '--output-directory', str(args.output / 'profiler'),
               '--'] + server
    if args.profiler == 'none':
        command = server
    (args.output / 'command.json').write_text(json.dumps(command, indent=2))
    first = (args.fixtures / 'first.sql').read_text()
    valid_ids = {int(x) for x in (args.fixtures / 'valid_ids.txt').read_text().splitlines()}
    with (args.output / 'server.out').open('w') as log:
        profiler = subprocess.Popen(command, env=environment, cwd=args.base_dir,
                                    stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
    server_pid = None
    try:
        deadline = time.monotonic() + 120
        while True:
            ready = subprocess.run(['mysql', '-h127.0.0.1', f'-P{args.port}', '-uroot',
                                    '-N', '-e', 'SELECT 1'], capture_output=True)
            if ready.returncode == 0:
                break
            if profiler.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError('Profiled server failed to become ready')
            time.sleep(1)
        server_pid = int((args.base_dir / 'run/seekdb.pid').read_text().strip())
        actual = (pathlib.Path('/proc') / str(server_pid) / 'cmdline').read_bytes().split(b'\0')[0]
        if actual != str(args.binary.resolve()).encode():
            server_pid = None
            raise RuntimeError('Server PID does not match the requested binary')
        result = dict(scope='public sample SQL functional check; not timing', profiler=args.profiler)
        for name, database in [('gpu_reload', args.gpu_database), ('cpu_reload', args.cpu_database)]:
            connection = Connection('127.0.0.1', args.port, 'root', '', database)
            try:
                cursor = connection.cursor()
                cursor.execute(first)
                ids = [int(row[0]) for row in cursor.fetchall()]
                if len(ids) != 10 or len(set(ids)) != 10 or not set(ids) <= valid_ids:
                    raise RuntimeError(name + ' result integrity failed')
                result[name] = ids
            finally:
                connection.close()
        database = 'gpu_profile_' + uuid.uuid4().hex[:12]
        tsv = (args.fixtures / 'items.tsv').resolve()
        if "'" in str(tsv):
            raise ValueError('Fixture path cannot contain a SQL quote')
        setup = (f'CREATE DATABASE {database}; USE {database}; '
                 'CREATE TABLE items(id BIGINT PRIMARY KEY,v VECTOR(128)); '
                 f"LOAD DATA LOCAL INFILE '{tsv}' INTO TABLE items "
                 "FIELDS TERMINATED BY '\\t' LINES TERMINATED BY '\\n' (id,v);")
        subprocess.run(['mysql', '--local-infile=1', '-h127.0.0.1', f'-P{args.port}',
                        '-uroot', '-e', setup], check=True)
        connection = Connection('127.0.0.1', args.port, 'root', '', database)
        try:
            cursor = connection.cursor()
            cursor.execute('SET ob_query_timeout=300000000')
            cursor.execute((args.fixtures / 'ddl-gpu.sql').read_text())
            cursor.execute(first)
            ids = [int(row[0]) for row in cursor.fetchall()]
            if ids != result['gpu_reload']:
                raise RuntimeError('Fresh-build and reloaded first-query IDs differ')
            result['fresh_gpu_build'] = ids
        finally:
            connection.close()
        result['result_integrity'] = True
        (args.output / 'sql-smoke.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2), flush=True)
    finally:
        if server_pid:
            os.kill(server_pid, signal.SIGTERM)
        elif profiler.poll() is None:
            profiler.terminate()
        status = profiler.wait(timeout=30)
        (args.output / 'exit-status.json').write_text(json.dumps({'returncode': status}))
        if status not in (0, -signal.SIGTERM):
            raise RuntimeError(f'Server/profiler exited abnormally: {status}')


if __name__ == '__main__':
    main()
