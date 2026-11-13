"""Regression tests for safe checkpoint/registry handling in the cloud trainer."""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load_module(name: str, relative_path: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative_path)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


class TrainingSafetyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = load_module("cortex_wikipedia_runner_test", "cloud/entrenar_wikipedia_continuo.py")
        cls.macro = load_module("cortex_macro_colab_test", "cortex-lm/tools/entrenar_macro_colab.py")

    def test_utf8_split_respects_byte_target_and_reconstructs_source(self):
        text = ("Párrafo con ñ, acentos y símbolos 🙂.\n\n" * 50) + "fin"
        chunks = self.macro.separar_texto_por_bytes(text, 128)
        self.assertEqual("".join(chunks), text)
        for chunk in chunks:
            chunk.encode("utf-8").decode("utf-8")
            self.assertLessEqual(len(chunk.encode("utf-8")), 128 + 8000 + 4)

    def test_registry_write_replaces_complete_json(self):
        with tempfile.TemporaryDirectory() as td:
            registry = Path(td) / "nested" / "run.json"
            self.macro.guardar_registro(registry, {"completed": ["batch-1"]})
            self.assertEqual(json.loads(registry.read_text(encoding="utf-8")), {"completed": ["batch-1"]})
            self.assertFalse(registry.with_name(registry.name + ".tmp").exists())

    def test_macro_shadow_failure_preserves_original_checkpoint(self):
        with tempfile.TemporaryDirectory() as td:
            model = Path(td) / "model.bin"
            model.write_bytes(b"NUB3original")

            def fail(cmd, _cwd):
                Path(cmd[2]).write_bytes(b"NUB3partial")
                raise RuntimeError("injected failure")

            with patch.object(self.macro, "run_cmd", side_effect=fail):
                with self.assertRaises(RuntimeError):
                    self.macro.actualizar_checkpoint_sobre_copia(
                        ["cerebro", "alimentar", str(model), "batch.txt"], model, td
                    )
            self.assertEqual(model.read_bytes(), b"NUB3original")
            self.assertFalse(model.with_name(model.name + ".candidate").exists())

    def test_macro_shadow_success_promotes_candidate(self):
        with tempfile.TemporaryDirectory() as td:
            model = Path(td) / "model.bin"
            model.write_bytes(b"NUB3original")

            def succeed(cmd, _cwd):
                Path(cmd[2]).write_bytes(b"NUB3updated")

            with patch.object(self.macro, "run_cmd", side_effect=succeed):
                self.macro.actualizar_checkpoint_sobre_copia(
                    ["cerebro", "alimentar", str(model), "batch.txt"], model, td
                )
            self.assertEqual(model.read_bytes(), b"NUB3updated")
            self.assertFalse(model.with_name(model.name + ".candidate").exists())

    def test_download_only_flag_never_starts_training(self):
        runner = self.runner
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            runner.LOCAL_MODEL = root / "model.bin"
            runner.LOCAL_REGISTRO = root / "registro.json"
            runner.get_token = lambda: "test-secret-placeholder"

            def fake_download(filename, destination, _token):
                destination.parent.mkdir(parents=True, exist_ok=True)
                if filename == runner.HF_MODEL_FILENAME:
                    destination.write_bytes(b"NUB3test")
                else:
                    destination.write_text("{}", encoding="utf-8")
                return True

            runner.hf_download = fake_download
            with patch.object(runner, "entrenar_lote", side_effect=AssertionError("--solo-descargar must not train")):
                runner.main(["--solo-descargar"])
            self.assertTrue(runner.LOCAL_MODEL.is_file())

    def test_failed_training_keeps_original_checkpoint(self):
        runner = self.runner
        with tempfile.TemporaryDirectory() as td:
            runner.LOCAL_MODEL = Path(td) / "model.bin"
            runner.LOCAL_MODEL.write_bytes(b"NUB3original")
            runner.CEREBRO_BIN = Path("/not/used/cerebro")

            def fail_on_candidate(cmd, **_kwargs):
                Path(cmd[2]).write_bytes(b"NUB3partial")
                return SimpleNamespace(returncode=1, stdout="", stderr="injected failure")

            with patch.object(runner.subprocess, "run", side_effect=fail_on_candidate):
                self.assertFalse(runner.entrenar_lote("texto", 1))
            self.assertEqual(runner.LOCAL_MODEL.read_bytes(), b"NUB3original")
            self.assertFalse(runner.LOCAL_MODEL.with_name(runner.LOCAL_MODEL.name + ".pending").exists())

    def test_successful_training_promotes_candidate(self):
        runner = self.runner
        with tempfile.TemporaryDirectory() as td:
            runner.LOCAL_MODEL = Path(td) / "model.bin"
            runner.LOCAL_MODEL.write_bytes(b"NUB3original")
            runner.CEREBRO_BIN = Path("/not/used/cerebro")

            def succeed_on_candidate(cmd, **_kwargs):
                Path(cmd[2]).write_bytes(b"NUB3updated")
                return SimpleNamespace(returncode=0, stdout="train=1.2 bpc\n[COMPLETADO]", stderr="")

            with patch.object(runner.subprocess, "run", side_effect=succeed_on_candidate):
                self.assertTrue(runner.entrenar_lote("texto", 2))
            self.assertEqual(runner.LOCAL_MODEL.read_bytes(), b"NUB3updated")
            self.assertFalse(runner.LOCAL_MODEL.with_name(runner.LOCAL_MODEL.name + ".pending").exists())


if __name__ == "__main__":
    unittest.main()
