"""Generate independent Gaussian FP32 diagnostics, never official or hidden inputs."""
import argparse
import hashlib
import json
import pathlib
import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--dimension', type=int, choices=[128, 256], default=256)
    parser.add_argument('--rows', type=int, default=250000)
    parser.add_argument('--nq', type=int, default=64)
    parser.add_argument('--seed', type=int, default=20261104)
    parser.add_argument('--query-seed', type=int, help='Use a separate RNG for held-out queries')
    args = parser.parse_args()
    if args.rows < 256 or args.nq < 1:
        parser.error('Require at least 256 base rows and one query')
    args.output.mkdir(parents=True, exist_ok=False)
    rng = np.random.default_rng(args.seed)
    rng.standard_normal((args.rows, args.dimension), dtype=np.float32).astype('<f4', copy=False).tofile(args.output / 'base.f32')
    query_rng = rng if args.query_seed is None else np.random.default_rng(args.query_seed)
    query_rng.standard_normal((args.nq, args.dimension), dtype=np.float32).astype('<f4', copy=False).tofile(args.output / 'query.f32')
    record = dict(scope='Independent isotropic Gaussian; not hidden or official data',
                  dataset='LocalGaussian', dimension=args.dimension, rows=args.rows,
                  nq=args.nq, seed=args.seed, query_seed=args.query_seed,
                  numpy_version=np.__version__)
    for name in ['base.f32', 'query.f32']:
        with (args.output / name).open('rb') as source:
            record[name + '_sha256'] = hashlib.file_digest(source, 'sha256').hexdigest()
    (args.output / 'fixture.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
