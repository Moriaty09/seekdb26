"""Check the FP32-tie comparator without requiring GPU libraries."""
import unittest
import numpy as np
from gpu_smoke import assert_equivalent_results


class ResultEquivalenceTest(unittest.TestCase):
    def test_rounding_tie_permutation(self):
        expected_ids = np.array([[7, 3, 9]], dtype=np.uint32)
        actual_ids = np.array([[3, 7, 9]], dtype=np.uint32)
        expected_dist = np.array([[1.5, 1.5, 2]], dtype=np.float32)
        actual_dist = expected_dist.copy()
        actual_dist[0, 1] = np.nextafter(np.float32(1.5), np.float32(2))
        assert_equivalent_results(actual_ids, actual_dist, expected_ids, expected_dist)

    def test_different_set_rejected(self):
        with self.assertRaises(AssertionError):
            assert_equivalent_results(np.array([[7, 4]]), np.array([[1., 1.]]),
                                      np.array([[7, 3]]), np.array([[1., 1.]]))

    def test_distinct_distance_permutation_rejected(self):
        with self.assertRaises(AssertionError):
            assert_equivalent_results(np.array([[3, 7]]), np.array([[1., 2.]]),
                                      np.array([[7, 3]]), np.array([[1., 2.]]))

    def test_changed_distance_rejected(self):
        with self.assertRaises(AssertionError):
            assert_equivalent_results(np.array([[7, 3]]), np.array([[1., 2.1]]),
                                      np.array([[7, 3]]), np.array([[1., 2.]]))


if __name__ == '__main__':
    unittest.main()
