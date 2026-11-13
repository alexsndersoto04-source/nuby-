#!/usr/bin/env python3
"""Evalúa un checkpoint sin modificarlo y registra baseline + hashes.

Ejemplo en Colab o local:
  python3 tools/evaluar_reproducible.py \
    --modelo data/cerebro_macro_1m.bin \
    --train /content/train_wikipedia.txt \
    --holdout /content/holdout_wikipedia.txt \
    --dataset-label 'wikimedia/wikipedia:20231101.es' \
    --salida /content/evaluacion.json

El holdout debe ser independiente del entrenamiento. El manifiesto prueba
identidad de archivos, no que el dataset de origen sea realmente inédito para
un checkpoint histórico; esa procedencia debe describirse en --dataset-label.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path

from baseline_ngram import witten_bell_holdout

BPC_RE = re.compile(r"HOLDOUT BPC \((\d+) chars\): ([0-9]+(?:\.[0-9]+)?) bpc")


def sha256_file(path: Path, chunk_size: int = 8 * 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(chunk_size), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_cortex_bpc(output: str) -> tuple[int, float]:
    match = BPC_RE.search(output)
    if not match:
        raise ValueError("No se encontró la línea HOLDOUT BPC en la salida de cerebro evaluar")
    return int(match.group(1)), float(match.group(2))


def atomic_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(data, stream, ensure_ascii=False, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--modelo", required=True, type=Path)
    parser.add_argument("--train", required=True, type=Path, help="Solo se usa para baseline; no se modifica")
    parser.add_argument("--holdout", required=True, type=Path)
    parser.add_argument("--binario", type=Path, default=Path(__file__).resolve().parents[1] / "bin" / "cerebro")
    parser.add_argument("--salida", required=True, type=Path)
    parser.add_argument("--dataset-label", default="no especificado")
    parser.add_argument("--metadata-json", type=Path,
                        help="Metadatos de origen/split (p. ej. IDs y títulos de artículos), sin secretos")
    parser.add_argument("--max-order", type=int, default=4)
    parser.add_argument("--min-holdout-bytes", type=int, default=16_000)
    args = parser.parse_args(argv)

    model, train_path, holdout_path, binary, output_path = (
        p.expanduser().resolve() for p in
        (args.modelo, args.train, args.holdout, args.binario, args.salida)
    )
    metadata_path = args.metadata_json.expanduser().resolve() if args.metadata_json else None
    required = [(model, "modelo"), (train_path, "train"), (holdout_path, "holdout"), (binary, "binario")]
    if metadata_path:
        required.append((metadata_path, "metadata-json"))
    for path, label in required:
        if not path.is_file():
            parser.error(f"No existe {label}: {path}")
    if args.max_order < 1 or args.min_holdout_bytes < 0:
        parser.error("--max-order debe ser >= 1 y --min-holdout-bytes >= 0")

    train = train_path.read_bytes()
    holdout = holdout_path.read_bytes()
    if not train or not holdout:
        parser.error("train y holdout deben ser archivos no vacíos")
    if len(holdout) < args.min_holdout_bytes:
        parser.error(
            f"holdout demasiado pequeño: {len(holdout)} bytes; mínimo {args.min_holdout_bytes}. "
            "Solo para smoke, especifica --min-holdout-bytes 0."
        )

    inputs = {"model": model, "train": train_path, "holdout": holdout_path, "binary": binary}
    if metadata_path:
        inputs["metadata"] = metadata_path
    metadata = json.loads(metadata_path.read_text(encoding="utf-8")) if metadata_path else None
    before = {name: {"path": str(path), "size_bytes": path.stat().st_size, "sha256": sha256_file(path)}
              for name, path in inputs.items()}

    command = [str(binary), "evaluar", str(model), str(holdout_path)]
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode != 0:
        raise SystemExit(f"cerebro evaluar terminó con código {result.returncode}:\n{result.stderr[-4000:]}")
    measured_bytes, cortex_bpc = parse_cortex_bpc(result.stdout)
    baseline = witten_bell_holdout(train, holdout, args.max_order)

    after = {name: {"size_bytes": path.stat().st_size, "sha256": sha256_file(path)}
             for name, path in inputs.items()}
    unchanged = all(before[name]["sha256"] == after[name]["sha256"] and
                    before[name]["size_bytes"] == after[name]["size_bytes"] for name in inputs)
    if not unchanged:
        raise SystemExit("Un archivo de entrada cambió durante la evaluación; no se genera manifiesto válido.")

    report = {
        "schema": "cortex-lm-evaluation-v1",
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "dataset_label": args.dataset_label,
        "split_metadata": metadata,
        "protocol": {
            "task": "next-byte prediction",
            "holdout_bytes": len(holdout),
            "cortex_reported_chars": measured_bytes,
            "minimum_holdout_bytes": args.min_holdout_bytes,
            "minimum_size_passed": len(holdout) >= args.min_holdout_bytes,
            "training_file_used_only_for_baseline": True,
            "inputs_unchanged": unchanged,
            "note": "Hashes establish file identity, not historical novelty relative to this checkpoint.",
        },
        "files": before,
        "cortex_lm": {"bpc": cortex_bpc, "command": command},
        "baseline": {
            "name": "Witten-Bell byte n-gram",
            "max_order": args.max_order,
            "bpc_by_order": {str(order): score for order, score in enumerate(baseline, start=1)},
            "best_order": min(range(1, len(baseline) + 1), key=lambda order: baseline[order - 1]),
            "best_bpc": min(baseline),
            "cortex_minus_best_baseline_bpc": cortex_bpc - min(baseline),
        },
        "cortex_stdout_tail": result.stdout[-4000:],
        "cortex_stderr_tail": result.stderr[-2000:],
    }
    atomic_json(output_path, report)
    print(json.dumps({
        "manifest": str(output_path),
        "holdout_bytes": len(holdout),
        "cortex_bpc": cortex_bpc,
        "witten_bell_bpc_by_order": report["baseline"]["bpc_by_order"],
        "inputs_unchanged": unchanged,
        "minimum_size_passed": report["protocol"]["minimum_size_passed"],
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
