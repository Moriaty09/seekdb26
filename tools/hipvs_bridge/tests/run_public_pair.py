"""Run an isolated public-sample SQL pair using already source-built binaries.

Does not delete runtime directories, install dependencies, or certify official
gates. Each server starts with a fresh directory and is stopped gracefully.
"""
import argparse
import json
import os
import pathlib
import signal
import shutil
import socket
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['baseline-bin', 'baseline-bridge', 'candidate-bin', 'candidate-bridge',
                 'fixtures', 'source-patch', 'output']:
        parser.add_argument('--' + name, type=pathlib.Path, required=True)
    parser.add_argument('--order', choices=['baseline,candidate', 'candidate,baseline'],
                        default='baseline,candidate')
    parser.add_argument('--port', type=int, default=2881)
    args = parser.parse_args()
    fixture = json.loads((args.fixtures / 'fixture.json').read_text())
    if fixture['dataset'] != 'PublicSample10K':
        parser.error('Only public diagnostic fixtures are supported')
    if shutil.disk_usage(args.output.parent).free < 30 * 1024 ** 3:
        parser.error('Require at least 30 GiB free for two fresh runtime directories')
    with socket.socket() as sock:
        if sock.connect_ex(('127.0.0.1', args.port)) == 0:
            parser.error('Port is occupied; leave the existing service untouched')
    args.output.mkdir(parents=True, exist_ok=False)
    mysql = ['mysql', '--local-infile=1', '-h127.0.0.1', f'-P{args.port}', '-uroot', '-N']
    benchmark = pathlib.Path(__file__).resolve().parent.parent / 'benchmark_sql.py'
    for variant in args.order.split(','):
        binary = getattr(args, variant + '_bin').resolve()
        bridge = getattr(args, variant + '_bridge').resolve()
        base = (args.output / ('run-' + variant)).resolve()
        base.mkdir()
        environment = dict(os.environ, LD_LIBRARY_PATH=str(bridge.parent) +
                           ':/opt/hipvs/lib:/opt/rocm/lib', MYSQL_PORT=str(args.port))
        server_args = [str(binary), '--base-dir=' + str(base), f'--port={args.port}',
                       '--nodaemon', '--parameter', '_memory_budget=16G',
                       '--parameter', 'datafile_size=8G', '--parameter', 'datafile_maxsize=12G',
                       '--parameter', 'log_disk_size=4G', '--log-level', 'INFO']
        (args.output / f'{variant}-server-command.json').write_text(json.dumps(server_args, indent=2))
        with (args.output / f'{variant}-server.out').open('w') as log:
            server = subprocess.Popen(server_args, cwd=base, env=environment, stdout=log,
                                      stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 120
            while True:
                ready = subprocess.run(mysql + ['-e', 'SELECT 1'], capture_output=True)
                if ready.returncode == 0:
                    break
                if server.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('Server did not become ready; see server output')
                time.sleep(1)
            tsv = (args.fixtures / 'items.tsv').resolve()
            if "'" in str(tsv):
                raise ValueError('Fixture path cannot contain a SQL quote')
            setup = ("CREATE DATABASE bench_public; USE bench_public; "
                     "ALTER SYSTEM SET ob_vector_memory_limit_percentage=30; "
                     "CREATE TABLE items(id BIGINT PRIMARY KEY,v VECTOR(128)); "
                     f"LOAD DATA LOCAL INFILE '{tsv}' INTO TABLE items "
                     "FIELDS TERMINATED BY '\\t' LINES TERMINATED BY '\\n' (id,v); "
                     "SELECT COUNT(*) FROM items;")
            imported = subprocess.run(mysql + ['-e', setup], capture_output=True, text=True)
            (args.output / f'{variant}-import.log').write_text(imported.stdout + imported.stderr)
            if imported.returncode or imported.stdout.strip() != str(fixture['rows']):
                raise RuntimeError('Import failed or row count differs')
            command = [sys.executable, str(benchmark), '--client', 'mysqlclient', '--variant',
                       variant + '-gpu', '--dataset', fixture['dataset'], '--dimension',
                       str(fixture['dimension']), '--rows', str(fixture['rows']), '--database',
                       'bench_public', '--ddl', str(args.fixtures / 'ddl-gpu.sql'),
                       '--first-query', str(args.fixtures / 'first.sql'), '--queries',
                       str(args.fixtures / 'queries.jsonl'), '--valid-ids',
                       str(args.fixtures / 'valid_ids.txt'), '--data-sha256', fixture['tsv_sha256'],
                       '--server-commit', '634888d9b037153fa143d2c913ba4b25503e1d62',
                       '--server-binary', str(binary), '--environment',
                       str(args.fixtures / 'environment.json'), '--output',
                       str(args.output / f'{variant}.json')]
            if variant == 'candidate':
                command += ['--source-patch', str(args.source_patch)]
            (args.output / f'{variant}-benchmark-command.json').write_text(json.dumps(command, indent=2))
            print('START', variant, flush=True)
            subprocess.run(command, env=environment, check=True)
            print((args.output / f'{variant}.json').read_text(), flush=True)
            (args.output / f'{variant}-process-status.txt').write_text(
                (pathlib.Path('/proc') / str(server.pid) / 'status').read_text())
        finally:
            if server.poll() is None:
                server.terminate()
                status = server.wait(timeout=30)
                (args.output / f'{variant}-exit-status.json').write_text(json.dumps({'returncode': status}))
                if status not in (0, -signal.SIGTERM):
                    raise RuntimeError(f'Server exited abnormally: {status}')


if __name__ == '__main__':
    main()
