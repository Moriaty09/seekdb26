"""Bridge-only scale diagnostic; excludes SQL DDL and snapshot persistence time."""
import argparse
import ctypes
import hashlib
import json
import pathlib
import threading
import time
import numpy as np


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['library', 'fixtures', 'output']:
        parser.add_argument('--' + name, type=pathlib.Path, required=True)
    parser.add_argument('--vram-file', type=pathlib.Path, required=True)
    args = parser.parse_args()
    metadata = json.loads((args.fixtures / 'fixture.json').read_text())
    dimension = metadata['dimension']
    base = np.fromfile(args.fixtures / 'base.f32', dtype='<f4').reshape(-1, dimension)
    probes = np.fromfile(args.fixtures / 'query.f32', dtype='<f4').reshape(-1, dimension)
    assert len(base) == metadata['rows'] and len(probes) == metadata['nq']
    assert np.isfinite(base).all() and np.isfinite(probes).all()
    # Exact FP64 ground truth, computed entirely outside the measured interval.
    reference = base.astype(np.float64)
    truth = []
    for query in probes:
        difference = reference - query.astype(np.float64)
        squared = np.einsum('ij,ij->i', difference, difference)
        truth.append(np.argsort(squared, kind='stable')[:100])
    del difference, squared, reference
    lib = ctypes.CDLL(str(args.library.resolve()))
    pointer = ctypes.c_void_p
    lib.seekdb_cuvs_build.argtypes = [pointer, ctypes.c_long, ctypes.c_long]
    lib.seekdb_cuvs_build.restype = pointer
    lib.seekdb_cuvs_search.argtypes = [pointer, pointer, ctypes.c_long, ctypes.c_long, pointer, pointer]
    lib.seekdb_cuvs_search.restype = ctypes.c_int
    lib.seekdb_cuvs_free.argtypes = [pointer]
    lib.seekdb_cuvs_free.restype = None
    hip = ctypes.CDLL('/opt/rocm/lib/libamdhip64.so')
    hip.hipDeviceSynchronize.restype = ctypes.c_int
    hip.hipDeviceGetPCIBusId.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    hip.hipDeviceGetPCIBusId.restype = ctypes.c_int
    pci = ctypes.create_string_buffer(64)
    assert hip.hipDeviceGetPCIBusId(pci, len(pci), 0) == 0
    actual_counter = pathlib.Path('/sys/bus/pci/devices') / pci.value.decode().lower() / 'mem_info_vram_used'
    assert actual_counter.is_file(), actual_counter
    requested_counter = str(args.vram_file)
    args.vram_file = actual_counter
    peak = [int(args.vram_file.read_text())]
    finished = threading.Event()

    def sample():
        while not finished.wait(0.01):
            peak[0] = max(peak[0], int(args.vram_file.read_text()))

    sampler = threading.Thread(target=sample)
    sampler.start()
    handle = None
    result = dict(scope='synthetic bridge-only diagnostic; not SQL or official scoring',
                  fixtures=metadata, library_sha256=digest(args.library),
                  result_integrity=False, sampled_vram_interval_seconds=0.01)
    result['gpu_pci_bus_id'] = pci.value.decode()
    result['sampled_vram_counter'] = str(actual_counter)
    result['requested_vram_counter'] = requested_counter
    try:
        def search(query, k):
            ids = np.empty(k, dtype=np.uint32)
            distances = np.empty(k, dtype=np.float32)
            assert lib.seekdb_cuvs_search(handle, query.ctypes.data, 1, k, ids.ctypes.data, distances.ctypes.data) == 0
            assert len(set(ids.tolist())) == k and np.all(ids < len(base))
            assert np.isfinite(distances).all() and np.all(distances >= 0)
            return ids

        start = time.perf_counter()
        handle = lib.seekdb_cuvs_build(base.ctypes.data, len(base), dimension)
        assert handle and hip.hipDeviceSynchronize() == 0
        search(probes[0], 10)
        result['bridge_build_and_first_query_seconds'] = time.perf_counter() - start
        for k in [10, 100]:
            start = time.perf_counter()
            answers = [search(query, k) for query in probes]
            seconds = time.perf_counter() - start
            result[f'top{k}'] = dict(recall=float(np.mean([len(set(a) & set(t[:k])) / k
                                                          for a, t in zip(answers, truth)])),
                                    bridge_queries_per_second=len(probes)/seconds)
        result['result_integrity'] = True
    finally:
        if handle:
            lib.seekdb_cuvs_free(handle)
        finished.set()
        sampler.join()
        result['sampled_peak_vram_bytes'] = peak[0]
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
