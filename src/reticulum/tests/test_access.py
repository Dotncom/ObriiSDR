import unittest
from obrii_reticulum.access import validate_access


class Access(unittest.TestCase):
    def test_ifac_bounds_and_validation(self):
        result = validate_access({"lora0": {"network_name": "example", "passphrase": "test passphrase"}}, ["lora0"], "rnode")
        self.assertEqual(result["lora0"]["ifac_size"], 8)
        for settings in ({}, {"passphrase": ""}, {"passphrase": "None"}, {"passphrase": "x" * 1025},
                         {"passphrase": "test", "ifac_size_bits": True},
                         {"passphrase": "test", "ifac_size_bits": 65},
                         {"passphrase": "test", "ifac_size_bits": 9},
                         {"passphrase": "test", "unknown": 1}):
            with self.assertRaises(ValueError):
                validate_access({"lora0": settings}, ["lora0"], "rnode")
        with self.assertRaises(ValueError):
            validate_access({"missing": {"passphrase": "test"}}, ["lora0"], "rnode")
