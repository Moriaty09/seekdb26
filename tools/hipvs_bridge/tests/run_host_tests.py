"""Run host control-flow tests using a C++17 compiler; no ROCm installation needed."""
import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', default='g++')
parser.add_argument('--temp-dir', default=None)
args = parser.parse_args()
here = pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='seekdb-bridge-test-', dir=args.temp_dir) as tmp:
    binary = pathlib.Path(tmp) / 'test_workspace.exe'
    subprocess.run([args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-I' + str(here / 'mock_include'), str(here / 'test_workspace.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(pathlib.Path(tmp) / 'snapshot.bin')], check=True)
    # Compile the real adaptor registry/snapshot section, with only OB logging,
    # allocation and clock services stubbed. Keep it verbatim instead of copying
    # the production wire-format code into a separate test implementation.
    adaptor = (here.parents[2] / 'src/oblib/lib/vector/ob_vsag_adaptor.cpp').read_text(encoding='utf-8')
    start = adaptor.index('// ==== [hipVS/cuVS]')
    end = adaptor.index('}  // anonymous namespace', start) + len('}  // anonymous namespace')
    (pathlib.Path(tmp) / 'adaptor_snapshot_under_test.h').write_text(adaptor[start:end], encoding='utf-8')
    snapshot_binary = pathlib.Path(tmp) / 'test_adaptor_snapshot.exe'
    subprocess.run([args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-pthread', '-DOB_BUILD_CUVS', '-DSEEKDB_TEST_ADAPTOR_SNAPSHOT',
                    '-I' + str(here / 'mock_include'), '-I' + str(tmp),
                    str(here / 'test_workspace.cpp'), '-o', str(snapshot_binary)], check=True)
    subprocess.run([str(snapshot_binary), str(pathlib.Path(tmp) / 'snapshot.bin')], check=True, timeout=30)
    worker_binary = pathlib.Path(tmp) / 'test_cuvs_worker.exe'
    subprocess.run([args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    str(here / 'test_cuvs_worker.cpp'), '-o', str(worker_binary)], check=True)
    subprocess.run([str(worker_binary)], check=True, timeout=30)
    if sys.platform.startswith('linux'):
        budget_binary = pathlib.Path(tmp) / 'test_host_thread_budget'
        subprocess.run([args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-rdynamic', str(here / 'test_host_thread_budget.cpp'),
                        '-ldl', '-o', str(budget_binary)], check=True)
        clean = dict(os.environ)
        for name in ['OMP_NUM_THREADS', 'KMP_BLOCKTIME']:
            clean.pop(name, None)
        for settings in [{}, {'OMP_NUM_THREADS': '2'}, {'KMP_BLOCKTIME': '15'},
                         {'OMP_NUM_THREADS': '2', 'KMP_BLOCKTIME': '15'}]:
            subprocess.run([str(budget_binary)], env=dict(clean, **settings), check=True)
