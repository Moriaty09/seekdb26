"""Compare a local paired GPU run; optionally add the CPU diagnostic run."""
import argparse
import json
import math
import pathlib


def compare(baseline, candidate, cpu=None):
    if baseline.get('sql_client') != candidate.get('sql_client'):
        raise ValueError('Unpaired SQL client')
    for key in ['dataset', 'rows', 'dimension', 'nq', 'data_sha256', 'queries_sha256',
                'ids_sha256', 'ddl_sha256', 'first_query_sha256', 'environment']:
        if baseline[key] != candidate[key]:
            raise ValueError('Unpaired inputs/environment: ' + key)
    if baseline['variant'] != 'baseline-gpu' or candidate['variant'] != 'candidate-gpu':
        raise ValueError('Expected baseline-gpu and candidate-gpu')
    if baseline['server_commit'] != '634888d9b037153fa143d2c913ba4b25503e1d62':
        raise ValueError('Baseline must be frozen V5')
    if not baseline['result_integrity'] or not candidate['result_integrity']:
        raise ValueError('A run did not complete result-integrity validation')
    for run in [baseline, candidate]:
        for value in [run['build_seconds']] + [run[f'top{k}'][field] for k in [10, 100] for field in ['qps', 'recall', 'p95_ms']]:
            if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise ValueError('Invalid numeric measurement')
        if run['build_seconds'] == 0 or any(run[f'top{k}']['qps'] == 0 for k in [10, 100]):
            raise ValueError('Timing/QPS must be positive')
        if any(run[f'top{k}']['recall'] > 1 for k in [10, 100]):
            raise ValueError('Recall must be in [0, 1]')
    build_ratio = baseline['build_seconds'] / candidate['build_seconds']
    query_ratio = math.sqrt(math.prod(candidate[f'top{k}']['qps'] / baseline[f'top{k}']['qps'] for k in [10, 100]))
    lines = [f"# {candidate['dataset']} 本地配对测试", '',
             '| 指标 | V5 GPU | 优化 GPU |', '|---|---:|---:|',
             f"| DDL＋首次查询 / s | {baseline['build_seconds']:.6f} | {candidate['build_seconds']:.6f} |"]
    for k in [10, 100]:
        for field in ['qps', 'p95_ms', 'recall']:
            lines.append(f"| Top-{k} {field} | {baseline[f'top{k}'][field]:.6f} | {candidate[f'top{k}'][field]:.6f} |")
    lines += ['', f'构建加速比：{build_ratio:.6f}x；查询几何平均加速比：{query_ratio:.6f}x。', '',
              f"查询吞吐门槛：{'通过' if query_ratio >= 0.9 else '失败'}。"]
    if candidate['dataset'] not in ['SIFT250K', 'Hidden250K']:
        lines.append('非正式数据集：Recall 仅记录，不宣称通过正式门槛。')
    else:
        for k in [10, 100]:
            minimum = 0.95 if candidate['dataset'] == 'SIFT250K' else (0.90 if k == 10 else 0.85)
            actual = candidate[f'top{k}']['recall']
            ok = actual >= minimum and baseline[f'top{k}']['recall'] - actual <= 0.005 + 1e-12
            lines.append(f"Recall@{k} 绝对值和基线差值门槛：{'通过' if ok else '失败'}。")
    if cpu:
        if cpu.get('sql_client') != candidate.get('sql_client'):
            raise ValueError('CPU comparison SQL client mismatch')
        if cpu['variant'] != 'cpu-vsag' or not cpu['result_integrity']:
            raise ValueError('CPU run is invalid or incomplete')
        for key in ['dataset', 'rows', 'dimension', 'nq', 'data_sha256', 'queries_sha256', 'ids_sha256', 'first_query_sha256', 'environment']:
            if cpu[key] != candidate[key]:
                raise ValueError('CPU comparison input mismatch: ' + key)
        if not math.isfinite(cpu['build_seconds']) or cpu['build_seconds'] <= 0:
            raise ValueError('Invalid CPU build measurement')
        lines += ['', f"CPU VSAG DDL＋首次查询：{cpu['build_seconds']:.6f} s；",
                  f"CPU/优化 GPU 构建时间比：{cpu['build_seconds'] / candidate['build_seconds']:.6f}x（诊断项，不用于正式计分）。"]
    lines += ['', 'GPU 路径、显存/RAM 峰值、CPU 功能和稳定性仍须独立核验；上述结果不等于硬门槛全部通过。',
              '完整计分公式已按官方 AMD 专题页核对；使用 score_runs.py 汇总两个正式数据集，仅在全部门槛已核验时给出本地估分。',
              '', f"候选服务源码：`{candidate['server_commit']}`；二进制 SHA-256：`{candidate['server_binary_sha256']}`。"]
    if candidate.get('server_source_patch_sha256'):
        lines.append('候选在上述基线提交上应用的未提交源码补丁 SHA-256：`' +
                     candidate['server_source_patch_sha256'] + '`。')
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=pathlib.Path, required=True)
    parser.add_argument('--candidate', type=pathlib.Path, required=True)
    parser.add_argument('--cpu', type=pathlib.Path)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    read = lambda path: json.loads(path.read_text(encoding='utf-8'))
    report = compare(read(args.baseline), read(args.candidate), read(args.cpu) if args.cpu else None)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report, encoding='utf-8')
