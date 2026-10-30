"""Exercise the real bridge on public FP32 samples (requires existing NumPy).

This is a bridge functional test, not the competition's SQL benchmark.
Every invocation builds a fresh graph and creates a fresh temporary snapshot.
"""
import argparse
import concurrent.futures
import ctypes
import hashlib
import json
import pathlib
import tempfile
import time

import numpy as np


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=pathlib.Path, required=True)
    parser.add_argument('--base', type=pathlib.Path, required=True)
    parser.add_argument('--queries', type=pathlib.Path, required=True)
    parser.add_argument('--dimension', type=int, default=128)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    base = np.fromfile(args.base, dtype='<f4').reshape(-1, args.dimension)
    queries = np.fromfile(args.queries, dtype='<f4').reshape(-1, args.dimension)
    if len(base) < 100 or len(queries) < 1:
        parser.error('Require at least 100 base vectors and one query')
    if not np.isfinite(base).all() or not np.isfinite(queries).all():
        parser.error('Require finite input vectors')
    truth = []
    reference_base = base.astype(np.float64)
    for query in queries:
        squared = np.sum((reference_base - query.astype(np.float64)) ** 2, axis=1)
        truth.append(np.argsort(squared, kind='stable')[:100])
    lib = ctypes.CDLL(str(args.library.resolve()))
    pointer = ctypes.c_void_p
    lib.seekdb_cuvs_build.argtypes = [pointer, ctypes.c_long, ctypes.c_long]
    lib.seekdb_cuvs_build.restype = pointer
    lib.seekdb_cuvs_search.argtypes = [pointer, pointer, ctypes.c_long,
                                     ctypes.c_long, pointer, pointer]
    lib.seekdb_cuvs_search.restype = ctypes.c_int
    lib.seekdb_cuvs_free.argtypes = [pointer]
    lib.seekdb_cuvs_free.restype = None
    lib.seekdb_cuvs_serialize.argtypes = [pointer, ctypes.c_char_p]
    lib.seekdb_cuvs_serialize.restype = ctypes.c_int
    lib.seekdb_cuvs_deserialize.argtypes = [ctypes.c_char_p, ctypes.c_long, ctypes.c_long]
    lib.seekdb_cuvs_deserialize.restype = pointer
    hip = ctypes.CDLL('/opt/rocm/lib/libamdhip64.so')
    hip.hipDeviceSynchronize.argtypes = []
    hip.hipDeviceSynchronize.restype = ctypes.c_int

    def search(handle, query_batch, k):
        query_batch = np.ascontiguousarray(query_batch, dtype=np.float32)
        ids = np.empty((len(query_batch), k), dtype=np.uint32)
        distances = np.empty(ids.shape, dtype=np.float32)
        rc = lib.seekdb_cuvs_search(handle, query_batch.ctypes.data, len(query_batch),
                                   k, ids.ctypes.data, distances.ctypes.data)
        if rc:
            raise RuntimeError(f'GPU search failed: {rc}')
        if not np.isfinite(distances).all() or np.any(distances < 0):
            raise RuntimeError('Invalid distances')
        for answer, distance in zip(ids, distances):
            if len(set(answer.tolist())) != k or np.any(answer >= len(base)):
                raise RuntimeError('Invalid or duplicate IDs')
            if np.any(np.diff(distance) < -1e-3):
                raise RuntimeError('Distances are not ordered')
        return ids.copy(), distances.copy()

    result = dict(scope='public-sample bridge functional check; not SQL performance',
                  rows=len(base), dimension=args.dimension, nq=len(queries),
                  library_sha256=digest(args.library), base_sha256=digest(args.base),
                  queries_sha256=digest(args.queries), result_integrity=False)
    started = time.perf_counter()
    handle = lib.seekdb_cuvs_build(base.ctypes.data, len(base), args.dimension)
    if not handle:
        raise RuntimeError('GPU build failed')
    try:
        if hip.hipDeviceSynchronize():
            raise RuntimeError('GPU build synchronization failed')
        first, _ = search(handle, queries[:1], 10)
        result['bridge_build_and_first_query_seconds'] = time.perf_counter() - started
        reference = {}
        for k in [10, 100]:
            answers = []
            started = time.perf_counter()
            for query in queries:
                ids, _ = search(handle, query.reshape(1, -1), k)
                answers.append(ids[0])
            elapsed = time.perf_counter() - started
            reference[k] = np.asarray(answers)
            result[f'top{k}'] = dict(
                recall=float(np.mean([len(set(a) & set(t[:k])) / k
                                      for a, t in zip(answers, truth)])),
                bridge_queries_per_second=len(queries) / elapsed)
        np.testing.assert_array_equal(search(handle, queries[:1], 10)[0], first)
        # Exercise the transient batch allocation path too.
        batch, _ = search(handle, queries[:min(8, len(queries))], 100)
        np.testing.assert_array_equal(batch, reference[100][:len(batch)])
        with tempfile.TemporaryDirectory(prefix='seekdb-gpu-snapshot-') as tmp:
            snapshot = pathlib.Path(tmp) / 'graph.bin'
            if lib.seekdb_cuvs_serialize(handle, str(snapshot).encode()):
                raise RuntimeError('Snapshot serialization failed')
            result['snapshot_bytes'] = snapshot.stat().st_size
            lib.seekdb_cuvs_free(handle)
            handle = None
            handle = lib.seekdb_cuvs_deserialize(str(snapshot).encode(), len(base), args.dimension)
            if not handle:
                raise RuntimeError('Snapshot reload failed')
            for k in [10, 100]:
                restored, _ = search(handle, queries[:1], k)
                np.testing.assert_array_equal(restored[0], reference[k][0])
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                answers = list(pool.map(lambda i: search(handle, queries[i:i+1], 10)[0][0],
                                        range(min(16, len(queries)))))
            np.testing.assert_array_equal(np.asarray(answers), reference[10][:len(answers)])
        result.update(result_integrity=True, snapshot_reload=True, concurrent_search=True)
    finally:
        if handle:
            lib.seekdb_cuvs_free(handle)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
