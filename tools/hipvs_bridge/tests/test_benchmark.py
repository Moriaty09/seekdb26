import copy
import importlib.util
import json
import pathlib
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

HERE = pathlib.Path(__file__).resolve().parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE.parent / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


bench = load('benchmark_sql')
reporter = load('compare_runs')
scoring = load('score_runs')


class BenchmarkTests(unittest.TestCase):
    def formal_pairs(self, build_ratio=1.0, query_ratio=1.0):
        pairs = {}
        for name, dim, recall in [('SIFT250K', 128, 0.99), ('Hidden250K', 256, 0.91)]:
            b = dict(dataset=name, dimension=dim, rows=250000, nq=4096,
                     variant='baseline-gpu', server_commit='634888d9b037153fa143d2c913ba4b25503e1d62',
                     server_binary_sha256='0' * 64, result_integrity=True, build_seconds=30,
                     environment={}, data_sha256='0' * 64, queries_sha256='0' * 64,
                     ids_sha256='0' * 64, ddl_sha256='0' * 64, first_query_sha256='0' * 64,
                     top10=dict(qps=100, recall=recall, p95_ms=10),
                     top100=dict(qps=100, recall=recall, p95_ms=10))
            c = copy.deepcopy(b)
            c.update(variant='candidate-gpu', build_seconds=30 / build_ratio)
            for k in [10, 100]:
                c[f'top{k}']['qps'] *= query_ratio
            pairs[name] = (b, c)
        return pairs

    def test_scoring_baseline_and_saturation(self):
        evidence = dict.fromkeys(scoring.EXTERNAL_GATES, True)
        self.assertEqual(scoring.estimate(self.formal_pairs(), evidence)['local_score'], 4)
        self.assertEqual(scoring.estimate(self.formal_pairs(3, 1.5), evidence)['local_score'], 20)
        self.assertEqual(scoring.estimate(self.formal_pairs(6, 3), evidence)['local_score'], 20)
        self.assertIsNone(scoring.estimate(self.formal_pairs())['local_score'])
        evidence['gpu_path'] = False
        self.assertEqual(scoring.estimate(self.formal_pairs(3, 1.5), evidence)['local_score'], 0)

    def test_scoring_recall_and_query_gates(self):
        pairs = self.formal_pairs(3, 1.5)
        pairs['Hidden250K'][1]['top100']['recall'] = 0.904
        self.assertEqual(scoring.estimate(pairs)['local_score'], 0)
        pairs['Hidden250K'][1]['top100']['recall'] = 0.905
        self.assertEqual(scoring.estimate(pairs)['status'], 'unverified')
        pairs = self.formal_pairs(3, 0.89)
        self.assertEqual(scoring.estimate(pairs)['local_score'], 0)
        with self.assertRaises(ValueError):
            scoring.estimate({'SIFT250K': pairs['SIFT250K']})

    def test_scoring_weighted_geometric_mean(self):
        pairs = self.formal_pairs()
        pairs['SIFT250K'][1]['build_seconds'] = 15
        pairs['Hidden250K'][1]['build_seconds'] = 10
        self.assertAlmostEqual(scoring.estimate(pairs)['weighted_ratios']['build'], 2 ** 0.4 * 3 ** 0.6)

    def test_integrity_rejects_bad_ids(self):
        for rows in [[(0,), (0,)], [(0,)], [(0,), (99,)], [(0, 1), (1, 2)]]:
            with self.assertRaises(ValueError):
                bench.check_ids(rows, 2, {0, 1})
        self.assertEqual(bench.check_ids([(0,), (1,)], 2, {0, 1}), [0, 1])

    def run_fake_database(self, tmp):
        root = pathlib.Path(tmp)
        (root / 'ids').write_text('\n'.join(map(str, range(100))))
        probe = dict(sql_10='k10', sql_100='k100', truth_100=list(range(100)))
        (root / 'queries').write_text((json.dumps(probe) + '\n') * 4096)
        for name, value in [('ddl', 'ddl'), ('first', 'k10'), ('binary', 'host fake, not a server'), ('env', '{}')]:
            (root / name).write_text(value)

        class Cursor:
            def execute(self, sql):
                self.sql = sql

            def fetchall(self):
                return [(i,) for i in range(100 if self.sql == 'k100' else 10)]

            def close(self):
                pass

        class Connection:
            def cursor(self):
                return Cursor()

            def close(self):
                pass

        connector = types.ModuleType('mysql.connector')
        connector.connect = lambda **kwargs: Connection()
        mysql = types.ModuleType('mysql')
        mysql.connector = connector
        argv = ['benchmark_sql.py', '--variant', 'baseline-gpu', '--dataset', 'host-fixture',
                '--dimension', '128', '--rows', '100', '--database', 'fake',
                '--ddl', str(root / 'ddl'), '--first-query', str(root / 'first'),
                '--queries', str(root / 'queries'), '--valid-ids', str(root / 'ids'),
                '--data-sha256', '0' * 64, '--server-commit', '634888d9b037153fa143d2c913ba4b25503e1d62',
                '--server-binary', str(root / 'binary'), '--environment', str(root / 'env'),
                '--output', str(root / 'result.json')]
        with patch.dict(sys.modules, {'mysql': mysql, 'mysql.connector': connector}), patch.object(sys, 'argv', argv):
            bench.main()
        return json.loads((root / 'result.json').read_text())

    def test_4096_queries_and_pairing(self):
        with tempfile.TemporaryDirectory(dir=TEMP_DIR) as tmp:
            baseline = self.run_fake_database(tmp)
        self.assertTrue(baseline['result_integrity'])
        self.assertEqual(baseline['top10']['recall'], 1)
        self.assertEqual(baseline['top100']['recall'], 1)
        candidate = copy.deepcopy(baseline)
        candidate['variant'] = 'candidate-gpu'
        candidate['build_seconds'] /= 2
        report = reporter.compare(baseline, candidate)
        self.assertIn('2.000000x', report)
        self.assertIn('非正式数据集', report)
        candidate['sql_client'] = 'mysqlclient'
        with self.assertRaises(ValueError):
            reporter.compare(baseline, candidate)
        candidate['sql_client'] = baseline['sql_client']
        candidate['queries_sha256'] = 'mismatched'
        with self.assertRaises(ValueError):
            reporter.compare(baseline, candidate)
        candidate['queries_sha256'] = baseline['queries_sha256']
        candidate['top10']['qps'] = float('nan')
        with self.assertRaises(ValueError):
            reporter.compare(baseline, candidate)


if __name__ == '__main__':
    # Optional workspace temporary parent for restricted Windows environments.
    TEMP_DIR = sys.argv.pop(1) if len(sys.argv) > 1 else None
    unittest.main()
else:
    TEMP_DIR = None
