"""Tests for reproducible CORTEX-LM holdout manifests and byte baselines."""
import importlib.util
import math
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "cortex-lm" / "tools"
sys.path.insert(0, str(TOOLS))


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


BASELINE = load_module("cortex_ngram_baseline_test", TOOLS / "baseline_ngram.py")
EVALUATOR = load_module("cortex_reproducible_eval_test", TOOLS / "evaluar_reproducible.py")


class EvaluationManifestTests(unittest.TestCase):
    def test_unseen_bytes_receive_finite_probability(self):
        scores = BASELINE.witten_bell_holdout(b"abcabcabc", b"abc\xf0\x9f\x99\x82", max_order=3)
        self.assertEqual(len(scores), 3)
        self.assertTrue(all(math.isfinite(score) for score in scores))
        self.assertTrue(all(score > 0 for score in scores))

    def test_cortex_output_parser_reads_bpc_and_byte_length(self):
        output = "Cerebro cargado en 15.4 ms | 5 regiones\nHOLDOUT BPC (16347 chars): 3.2208 bpc\n"
        self.assertEqual(EVALUATOR.parse_cortex_bpc(output), (16347, 3.2208))
        with self.assertRaises(ValueError):
            EVALUATOR.parse_cortex_bpc("sin métrica")

    def test_atomic_json_write_leaves_complete_parseable_manifest(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "nested" / "evaluation.json"
            EVALUATOR.atomic_json(path, {"schema": "test-v1", "inputs_unchanged": True})
            import json
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")),
                             {"schema": "test-v1", "inputs_unchanged": True})
            self.assertEqual(list(path.parent.glob("*.tmp")), [])

    def test_sha256_file_is_stable(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "file.bin"
            path.write_bytes(b"NUB3test")
            self.assertEqual(EVALUATOR.sha256_file(path),
                             "ffd16ed50c9e26b5e88a985430cd87165d1674d06157b7115b7515c1847c5b85")


if __name__ == "__main__":
    unittest.main()
