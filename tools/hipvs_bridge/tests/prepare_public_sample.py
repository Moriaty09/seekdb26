"""Prepare the template's 10K public sample for local SQL diagnostics.

The 100 supplied probes repeat to fill the benchmark's 4096 SQL requests.
This fixture is not SIFT250K, Hidden250K, or an official scoring workload.
"""
import argparse
import hashlib
import json
import pathlib

import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=pathlib.Path, required=True)
    parser.add_argument('--queries', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--dimension', type=int, default=128)
    args = parser.parse_args()
    root = args.output
    root.mkdir(parents=True, exist_ok=False)
    base = np.fromfile(args.base, dtype='<f4').reshape(-1, args.dimension)
    queries = np.fromfile(args.queries, dtype='<f4').reshape(-1, args.dimension)
    # Non-contiguous external IDs exercise row-offset-to-SQL-ID mapping.
    ids = np.arange(len(base), dtype=np.int64) * 17 + 1009
    literal = lambda row: '[' + ','.join(format(float(x), '.9g') for x in row) + ']'
    with (root / 'items.tsv').open('w', encoding='utf-8', newline='\n') as f:
        for external_id, row in zip(ids, base):
            f.write(f'{external_id}\t{literal(row)}\n')
    (root / 'valid_ids.txt').write_text('\n'.join(map(str, ids)) + '\n', encoding='utf-8')
    truth = []
    reference = base.astype(np.float64)
    for query in queries:
        distance = np.sum((reference - query.astype(np.float64)) ** 2, axis=1)
        truth.append(ids[np.argsort(distance, kind='stable')[:100]].tolist())
    records = []
    for i in range(4096):
        q = i % len(queries)
        record = {f'sql_{k}': f"SELECT id FROM items ORDER BY l2_distance(v,'{literal(queries[q])}') APPROXIMATE LIMIT {k};"
                  for k in [10, 100]}
        record['truth_100'] = truth[q]
        records.append(record)
    (root / 'queries.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in records), encoding='utf-8')
    (root / 'first.sql').write_text(records[0]['sql_10'], encoding='utf-8')
    for variant, library in [('gpu', 'cuvs'), ('cpu', 'vsag')]:
        (root / f'ddl-{variant}.sql').write_text(
            'CREATE VECTOR INDEX idx_items ON items(v) WITH '
            f'(distance=l2,type=hnsw,lib={library},m=16,ef_construction=200,ef_search=64);\n',
            encoding='utf-8')
    metadata = dict(dataset='PublicSample10K', rows=len(base), dimension=args.dimension,
                    unique_probes=len(queries), nq=4096, probe_policy='cyclic repetition of public probes',
                    external_ids='row offset * 17 + 1009',
                    base_sha256=hashlib.sha256(args.base.read_bytes()).hexdigest(),
                    queries_sha256=hashlib.sha256(args.queries.read_bytes()).hexdigest(),
                    tsv_sha256=hashlib.sha256((root / 'items.tsv').read_bytes()).hexdigest())
    (root / 'fixture.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    main()
