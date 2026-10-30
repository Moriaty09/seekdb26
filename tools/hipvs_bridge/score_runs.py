"""Local AMD V5 score estimate. Official scores always come from the organizer."""
import argparse
import json
import math
import pathlib

from compare_runs import compare

EXTERNAL_GATES = (
    'gpu_path', 'index_integrity', 'cpu_path', 'stability', 'relative_vram_limit',
    'physical_vram_limit', 'host_memory_limit', 'build_constraints',
    'gpu_time_limit', 'v5_ancestry',
)


def estimate(pairs, evidence=None):
    if set(pairs) != {'SIFT250K', 'Hidden250K'}:
        raise ValueError('Both formal datasets are required; do not substitute hidden data')
    external = {name: (evidence or {}).get(name) for name in EXTERNAL_GATES}
    if any(value is not None and type(value) is not bool for value in external.values()):
        raise ValueError('External gates must be true, false or null')
    ratios, gates = {}, {}
    for name, (baseline, candidate) in pairs.items():
        if baseline['dataset'] != name or candidate['dataset'] != name:
            raise ValueError('Dataset label mismatch: ' + name)
        compare(baseline, candidate)  # Verify pairing, V5, integrity, finite metrics.
        if candidate['rows'] != 250000 or candidate['dimension'] != (128 if name == 'SIFT250K' else 256) or candidate['nq'] != 4096:
            raise ValueError('Unexpected formal dataset shape/query count: ' + name)
        build = baseline['build_seconds'] / candidate['build_seconds']
        query = math.exp(sum(math.log(candidate[f'top{k}']['qps'] / baseline[f'top{k}']['qps']) for k in [10, 100]) / 2)
        ratios[name] = {'build': build, 'query': query}
        gates[name + ':query_regression'] = query >= 0.90
        gates[name + ':result_integrity'] = candidate['result_integrity'] is True
        for k in [10, 100]:
            minimum = 0.95 if name == 'SIFT250K' else (0.90 if k == 10 else 0.85)
            recall = candidate[f'top{k}']['recall']
            gates[f'{name}:recall@{k}_minimum'] = recall >= minimum
            gates[f'{name}:recall@{k}_baseline_delta'] = baseline[f'top{k}']['recall'] - recall <= 0.005 + 1e-12
    weighted = {metric: math.exp(sum(weight * math.log(ratios[name][metric])
                for name, weight in [('SIFT250K', 0.4), ('Hidden250K', 0.6)]))
                for metric in ['build', 'query']}
    clamp = lambda x: max(0.0, min(1.0, x))
    build_points = 14 * clamp(math.log(weighted['build']) / math.log(3.0))
    query_points = 2 * clamp(math.log(weighted['query']) / math.log(1.5))
    failed = [name for name, passed in {**gates, **external}.items() if passed is False]
    pending = [name for name, passed in external.items() if passed is None]
    score = 0.0 if failed else (None if pending else round(4 + build_points + query_points, 3))
    return {'status': 'fail' if failed else ('unverified' if pending else 'local_gates_pass'),
            'local_score': score, 'failed_gates': failed, 'pending_gates': pending,
            'dataset_ratios': ratios, 'weighted_ratios': weighted,
            'build_points_before_gates': build_points, 'query_points_before_gates': query_points,
            'notice': 'Local diagnostic only. External gate evidence is operator supplied; official evaluator decides the score.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['sift-baseline', 'sift-candidate', 'hidden-baseline', 'hidden-candidate']:
        parser.add_argument('--' + name, type=pathlib.Path, required=True)
    parser.add_argument('--evidence', type=pathlib.Path, help='Operator-verified external gate booleans, not official certification')
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    read = lambda path: json.loads(path.read_text(encoding='utf-8'))
    result = estimate({'SIFT250K': (read(args.sift_baseline), read(args.sift_candidate)),
                       'Hidden250K': (read(args.hidden_baseline), read(args.hidden_candidate))},
                      read(args.evidence) if args.evidence else None)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
