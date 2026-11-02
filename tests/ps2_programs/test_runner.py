"""Acceptance checks for the shared native/reference result protocol."""
import configparser
from pathlib import Path
import struct
import unittest
from run import MAGIC, load_catalog, observations, reference_settings, select_cases


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

    def test_progressive_matrix_selection(self):
        catalog = load_catalog()
        basic = select_cases(catalog, max_level=1)
        self.assertEqual({case['name'] for case in basic}, {'scalar', 'direct_flow'})
        self.assertEqual(len(select_cases(catalog, ['callback_stack'], 2)), 1)
        with self.assertRaises(ValueError):
            select_cases(catalog, ['callback_queue'], 1)
        self.assertEqual({case['level'] for case in catalog['cases']}, {1, 2, 3, 4, 5})
        self.assertTrue(any(len(case['sources']) > 1 for case in catalog['cases']))

    def test_reference_modes_use_isolated_explicit_cpu_settings(self):
        for mode, expected in [('recompiler', True), ('interpreter', False)]:
            settings = configparser.ConfigParser()
            settings.read_string(reference_settings(Path('reference/bios.bin'), 45678, mode))
            cpu = settings['EmuCore/CPU/Recompiler']
            for key in ('EnableEE', 'EnableIOP', 'EnableVU0', 'EnableVU1'):
                self.assertEqual(cpu.getboolean(key), expected)
            self.assertEqual(settings['EmuCore'].getint('PINESlot'), 45678)
            self.assertEqual(settings['Filenames']['BIOS'], 'bios.bin')
        with self.assertRaises(ValueError):
            reference_settings(Path('reference/bios.bin'), 45678, 'unknown')


if __name__ == '__main__':
    unittest.main()
