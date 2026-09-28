"""Policy boundary and malformed API tests, without touching hardware."""
import importlib.util
import unittest
from unittest.mock import patch
from pathlib import Path
import tempfile

spec = importlib.util.spec_from_file_location('power', Path(__file__).with_name('santos-powerd.py'))
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)

class Policy(unittest.TestCase):
    def test_thresholds_and_inhibitors(self):
        c = p.DEFAULTS
        self.assertIsNone(p.due(c, 299, True, set()))
        self.assertEqual(p.due(c, 300, True, set()), 'off')
        self.assertIsNone(p.due(c, 500, True, {'screen'}))
        self.assertIsNone(p.due(c, 1799, False, set()))
        self.assertIsNone(p.due(c, 1800, False, set()))
        self.assertIsNone(p.due(c, 1900, False, {'suspend'}))
        self.assertIsNone(p.due(p.DEFAULTS, 9000, False, set()))
        self.assertIsNone(p.due(c | {'screen_off_seconds': 0}, 9999, True, set()))

    def test_settings_and_api(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(p, 'CONFIG', Path(folder)/'config.json'):
            power = p.Power()
            with self.assertRaises(ValueError):
                power.request([])
            for value in (-1, 86401, True, 1.5):
                with self.assertRaises(ValueError):
                    power.request({'method':'configure', 'settings':{'screen_off_seconds': value}})
            with patch.object(p.Path, 'exists', return_value=False):
                with self.assertRaises(ValueError):
                    p.validate(p.DEFAULTS | {'suspend_enabled': True})
            with self.assertRaises(ValueError):
                power.request({'method':'suspend'})
            power.request({'method':'configure', 'settings':{'screen_off_seconds':600}})
            self.assertEqual(p.Power().config['screen_off_seconds'], 600)
            result = power.request({'method':'inhibit', 'seconds':1, 'scopes':['screen']})
            self.assertEqual(len(power.inhibitors()), 1)
            with patch.object(p.time, 'monotonic', return_value=p.time.monotonic()+2):
                self.assertEqual(power.inhibitors(), [])
            with self.assertRaises(ValueError):
                power.request({'method':'inhibit', 'token':result['token']})

if __name__ == '__main__':
    unittest.main()
