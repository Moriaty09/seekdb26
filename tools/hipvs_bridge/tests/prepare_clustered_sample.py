"""Generate an explicitly synthetic scale fixture, unrelated to hidden inputs.

This is not SIFT250K, Hidden250K or a competition scoring dataset.
Requires the existing NumPy installation; never downloads or installs packages.
"""
import argparse
import hashlib
import json
import pathlib
import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dimension', type=int, choices=[128, 256], required=True)
    parser.add_argument('--rows', type=int, default=250000)
    parser.add_argument('--nq', type=int, default=64)
    parser.add_argument('--seed', type=int, default=20261102)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.rows < 256 or args.nq < 1:
        parser.error('Require at least 256 rows and one query')
    args.output.mkdir(parents=True, exist_ok=False)
    rng = np.random.default_rng(args.seed)
    centers = rng.normal(0, 3, (16, 16)).astype(np.float32)
    projection = (rng.normal(size=(16, args.dimension)) / 4).astype(np.float32)
    labels = rng.integers(0, len(centers), args.rows)
    latent = centers[labels] + rng.normal(0, 0.4, (args.rows, 16)).astype(np.float32)
    base = latent @ projection
    base += rng.normal(0, 0.003, base.shape).astype(np.float32)
    query_latent = centers[np.arange(args.nq) % len(centers)] + rng.normal(0, 0.4, (args.nq, 16)).astype(np.float32)
    queries = query_latent @ projection
    queries += rng.normal(0, 0.003, queries.shape).astype(np.float32)
    base.astype('<f4', copy=False).tofile(args.output / 'base.f32')
    queries.astype('<f4', copy=False).tofile(args.output / 'query.f32')
    record = dict(scope='synthetic scale diagnostic; not an official or hidden dataset',
                  dataset=f'SyntheticClusters{args.rows}x{args.dimension}', seed=args.seed,
                  rows=args.rows, dimension=args.dimension, nq=args.nq, latent_dimension=16,
                  clusters=16, latent_noise_std=0.4, output_noise_std=0.003,
                  numpy_version=np.__version__)
    for filename in ['base.f32', 'query.f32']:
        with (args.output / filename).open('rb') as source:
            record[filename + '_sha256'] = hashlib.file_digest(source, 'sha256').hexdigest()
    (args.output / 'fixture.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
