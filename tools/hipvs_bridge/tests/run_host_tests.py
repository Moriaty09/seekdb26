"""Run host control-flow tests using a C++17 compiler; no ROCm installation needed."""
import argparse
import pathlib
import subprocess
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
    worker_binary = pathlib.Path(tmp) / 'test_cuvs_worker.exe'
    subprocess.run([args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    str(here / 'test_cuvs_worker.cpp'), '-o', str(worker_binary)], check=True)
    subprocess.run([str(worker_binary)], check=True, timeout=30)
