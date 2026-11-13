"""Tests for Colab checkpoint download/upload guardrails."""
import importlib.util
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cortex_colab_sync_test", ROOT / "cloud/colab_sync.py")
sync = importlib.util.module_from_spec(spec)
assert spec and spec.loader
spec.loader.exec_module(sync)


class ColabSyncSafetyTests(unittest.TestCase):
    def test_valid_download_promotes_atomically_and_reports_hash(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "remote.bin"
            source.write_bytes(b"NUB3" + b"model payload")
            sync.LOCAL_MODEL = root / "nested" / "model.bin"
            fake_hub = types.ModuleType("huggingface_hub")
            fake_hub.hf_hub_download = lambda **_kwargs: str(source)
            with patch.dict(sys.modules, {"huggingface_hub": fake_hub}):
                sync.cmd_download("not-a-real-token")
            self.assertEqual(sync.LOCAL_MODEL.read_bytes(), source.read_bytes())
            self.assertFalse(sync.LOCAL_MODEL.with_name(sync.LOCAL_MODEL.name + ".download").exists())
            self.assertEqual(sync.verify_checkpoint(sync.LOCAL_MODEL), sync.verify_checkpoint(source))

    def test_invalid_download_preserves_existing_checkpoint(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "remote.bin"
            source.write_bytes(b"HTML error response")
            sync.LOCAL_MODEL = root / "model.bin"
            sync.LOCAL_MODEL.write_bytes(b"NUB3old valid model")
            fake_hub = types.ModuleType("huggingface_hub")
            fake_hub.hf_hub_download = lambda **_kwargs: str(source)
            with patch.dict(sys.modules, {"huggingface_hub": fake_hub}):
                with self.assertRaises(ValueError):
                    sync.cmd_download("not-a-real-token")
            self.assertEqual(sync.LOCAL_MODEL.read_bytes(), b"NUB3old valid model")
            self.assertFalse(sync.LOCAL_MODEL.with_name(sync.LOCAL_MODEL.name + ".download").exists())


if __name__ == "__main__":
    unittest.main()
