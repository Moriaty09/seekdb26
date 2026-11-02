"""Record local diagnostic settings without installing packages or loading hipVS."""
import argparse
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()

    def read(path):
        try:
            return pathlib.Path(path).read_text().strip()
        except OSError:
            return None

    def run(command):
        try:
            return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT, timeout=30).strip()
        except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
            return 'unavailable: ' + str(error)

    library = pathlib.Path('/opt/hipvs/lib/libcuvs_c.so')
    hipvs_sha256 = None
    if library.is_file():
        with library.open('rb') as source:
            hipvs_sha256 = hashlib.file_digest(source, 'sha256').hexdigest()
    record = dict(scope='local diagnostic environment; not hardware or gate certification',
                  platform=platform.platform(), cpu_max=read('/sys/fs/cgroup/cpu.max'),
                  memory_max=read('/sys/fs/cgroup/memory.max'),
                  cpu_affinity=sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None,
                  rocm=read('/opt/rocm/.info/version'), hipvs_library_sha256=hipvs_sha256,
                  openmp_environment={name: os.environ.get(name) for name in ['OMP_NUM_THREADS','KMP_BLOCKTIME']},
                  service=dict(memory_budget='16G',datafile_size='8G',datafile_maxsize='12G',log_disk_size='4G'),
                  gcc=run(['gcc','--version']), rust=run(['rustc','--version']),
                  cmake=run([shutil.which('cmake') or '/opt/venv/bin/cmake','--version']),
                  gpu=run([shutil.which('rocm-smi') or '/opt/rocm/bin/rocm-smi',
                           '--showproductname','--showbus','--showmeminfo','vram','--json']))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('x',encoding='utf-8') as output:
        json.dump(record,output,indent=2)
        output.write('\n')


if __name__ == '__main__':
    main()
