"""Acceptance checks for the shared native/reference result protocol."""
import struct
import unittest
from run import MAGIC, observations


class ResultProtocol(unittest.TestCase):
    def test_complete_results_and_rejected_false_success(self):
        case = dict(id=2, values=[20, 21, 20, 1])
        words = [MAGIC, 1, 2, 1, 4, 20, 21, 20, 1] + [0] * 12
        encode = lambda values: struct.pack('<21I', *values)
        self.assertEqual(observations(encode(words), case), case['values'])
        for index, value in [(0, 0), (1, 2), (2, 1), (3, 0), (4, 3), (5, 19)]:
            changed = words.copy()
            changed[index] = value
            with self.subTest(index=index), self.assertRaises(ValueError):
                observations(encode(changed), case)
        with self.assertRaises(ValueError):
            observations(encode(words)[:-1], case)


if __name__ == '__main__':
    unittest.main()
