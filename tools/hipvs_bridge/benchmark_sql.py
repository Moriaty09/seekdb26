"""Local offline-DDL benchmark using the repository's existing mysql.connector client.

The caller supplies SQL and ground truth. This script does not generate official
SQL, change index parameters, import data, clear caches, or certify GPU execution.
"""
import argparse
import hashlib
import json
import math
import os
import pathlib
import re
import time


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def check_ids(rows, k, valid_ids):
    if any(len(row) != 1 for row in rows):
        raise ValueError('SQL must return exactly one ID column')
    ids = [int(row[0]) for row in rows]
    if len(ids) != k or len(set(ids)) != k or not set(ids) <= valid_ids:
        raise ValueError('result_integrity: wrong cardinality, duplicate or invalid IDs')
    return ids


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant', choices=['baseline-gpu', 'candidate-gpu', 'cpu-vsag'], required=True)
    parser.add_argument('--client', choices=['connector', 'mysqlclient'], default='connector',
                        help='Use an existing Python connector or existing system libmysqlclient')
    parser.add_argument('--dataset', required=True)
    parser.add_argument('--dimension', type=int, required=True)
    parser.add_argument('--rows', type=int, default=250000)
    parser.add_argument('--database', required=True)
    parser.add_argument('--ddl', type=pathlib.Path, required=True)
    parser.add_argument('--first-query', type=pathlib.Path, required=True)
    parser.add_argument('--first-k', type=int, default=10)
    parser.add_argument('--queries', type=pathlib.Path, required=True)
    parser.add_argument('--valid-ids', type=pathlib.Path, required=True)
    parser.add_argument('--data-sha256', required=True, help='SHA-256 of the imported FP32 dataset file')
    parser.add_argument('--server-commit', required=True, help='Actual source revision of the running server')
    parser.add_argument('--server-binary', type=pathlib.Path, required=True)
    parser.add_argument('--source-patch', type=pathlib.Path,
                        help='For an uncommitted candidate, also record its complete source patch')
    parser.add_argument('--environment', type=pathlib.Path, required=True, help='Recorded hardware/software JSON')
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9a-f]{40}', args.server_commit):
        parser.error('--server-commit must be the full actual revision')
    if not re.fullmatch(r'[0-9a-f]{64}', args.data_sha256):
        parser.error('--data-sha256 must contain 64 lowercase hexadecimal characters')
    valid_ids = {int(line) for line in args.valid_ids.read_text().splitlines() if line.strip()}
    probes = [json.loads(line) for line in args.queries.read_text(encoding='utf-8').splitlines() if line.strip()]
    if len(valid_ids) != args.rows or len(probes) != 4096 or args.dimension <= 0:
        parser.error('Require the declared number of distinct input IDs, positive dimension, and 4096 probes')
    for probe in probes:
        truth = probe['truth_100']
        if len(truth) != 100 or len(set(truth)) != 100 or not set(truth) <= valid_ids:
            parser.error('Each probe requires 100 distinct valid exact-neighbor IDs in distance order')
        if not probe.get('sql_10') or not probe.get('sql_100'):
            parser.error('Each probe requires single-row-query SQL for Top-10 and Top-100')
    # All file reads/hashing are outside the timed region.
    ddl = args.ddl.read_text(encoding='utf-8')
    first_query = args.first_query.read_text(encoding='utf-8')
    result = {
        'variant': args.variant, 'dataset': args.dataset, 'dimension': args.dimension, 'rows': args.rows,
        'server_commit': args.server_commit, 'server_binary_sha256': sha256(args.server_binary),
        'server_source_patch_sha256': sha256(args.source_patch) if args.source_patch else None,
        'data_sha256': args.data_sha256, 'queries_sha256': sha256(args.queries),
        'ids_sha256': sha256(args.valid_ids), 'ddl_sha256': sha256(args.ddl),
        'first_query_sha256': sha256(args.first_query), 'nq': len(probes),
        'environment': json.loads(args.environment.read_text(encoding='utf-8')),
        'gpu_path': 'unverified: inspect independent profiler evidence',
        'resources': 'unverified: inspect external peak VRAM/RAM measurements',
        'result_integrity': False,
        'sql_client': args.client,
    }
    if args.client == 'mysqlclient':
        from mysqlclient_adapter import Connection as connect
    else:
        import mysql.connector  # Already referenced by tools/import_srs_data.py.
        connect = mysql.connector.connect
    connection = connect(
        host=os.environ.get('MYSQL_HOST', '127.0.0.1'),
        port=int(os.environ.get('MYSQL_PORT', '2881')),
        user=os.environ.get('MYSQL_USER', 'root'),
        password=os.environ.get('MYSQL_PWD', ''), database=args.database,
        autocommit=True,
    )
    try:
        cursor = connection.cursor()
        cursor.execute('SET ob_query_timeout=300000000')
        start = time.perf_counter()
        cursor.execute(ddl)
        ddl_end = time.perf_counter()
        cursor.execute(first_query)
        first_rows = cursor.fetchall()
        end = time.perf_counter()
        result.update(build_seconds=end - start, ddl_seconds=ddl_end - start,
                      first_query_seconds=end - ddl_end)
        check_ids(first_rows, args.first_k, valid_ids)
        for k in [10, 100]:
            latencies, answers = [], []
            group_start = time.perf_counter()
            for probe in probes:
                query_start = time.perf_counter()
                cursor.execute(probe[f'sql_{k}'])
                answers.append(cursor.fetchall())
                latencies.append(time.perf_counter() - query_start)
            elapsed = time.perf_counter() - group_start
            ids = [check_ids(rows, k, valid_ids) for rows in answers]
            recall = sum(len(set(answer) & set(probe['truth_100'][:k])) / k
                         for answer, probe in zip(ids, probes)) / len(probes)
            result[f'top{k}'] = {
                'qps': len(probes) / elapsed, 'elapsed_seconds': elapsed,
                'p95_ms': sorted(latencies)[math.ceil(0.95 * len(latencies)) - 1] * 1000,
                'recall': recall,
            }
        result['result_integrity'] = True
        cursor.close()
    finally:
        connection.close()
        # Preserve diagnostics on a failed run. Exceptions still produce a nonzero exit.
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
