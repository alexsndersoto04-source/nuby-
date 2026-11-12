#!/usr/bin/env python3
"""Baseline Witten–Bell de bytes, con split cronológico o train/holdout externos.

No ajusta parámetros mirando el test: cuenta únicamente el train y puntúa el
holdout. Cada byte UTF-8 es un símbolo, igual que en CORTEX-LM.

Uso:
  python3 tools/baseline_ngram.py data/corpus.txt [--split 0.8] [--max-order 4]
  python3 tools/baseline_ngram.py train.txt --holdout holdout.txt [--max-order 4]
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from math import log2
from pathlib import Path


def witten_bell_holdout(train: bytes, test: bytes, max_order: int = 4) -> list[float]:
    """Ajusta solo con ``train`` y devuelve BPC de cada orden sobre ``test``."""
    if not train or not test:
        raise ValueError("train y holdout deben contener bytes")
    if max_order < 1:
        raise ValueError("max_order debe ser >= 1")

    vocabulary = set(train)
    unigram = Counter(train)
    # context -> continuations -> frequency
    counts: list[dict[bytes, Counter[int]]] = [defaultdict(Counter) for _ in range(max_order + 1)]
    for i, target in enumerate(train):
        for order in range(1, max_order + 1):
            if i >= order:
                counts[order][train[i - order : i]][target] += 1

    def probability(target: int, history: bytes, order: int) -> float:
        if order == 0:
            alpha = 0.1
            return (unigram[target] + alpha) / (len(train) + alpha * len(vocabulary))

        followers = counts[order].get(history[-order:])
        if not followers:
            return probability(target, history, order - 1)
        total = sum(followers.values())
        distinct = len(followers)
        lower = probability(target, history, order - 1)
        return followers[target] / (total + distinct) + distinct / (total + distinct) * lower

    results: list[float] = []
    for order in range(1, max_order + 1):
        history = train[-order:]
        bits = 0.0
        for target in test:
            bits -= log2(max(probability(target, history, order), 1e-300))
            history = (history + bytes((target,)))[-order:]
        results.append(bits / len(test))
    return results


def witten_bell_scores(data: bytes, split: float = 0.8, max_order: int = 4) -> list[float]:
    """Compatibilidad: separa un único corpus cronológicamente y evalúa el final."""
    if not 0.0 < split < 1.0:
        raise ValueError("split debe estar entre 0 y 1")
    split_at = int(len(data) * split)
    return witten_bell_holdout(data[:split_at], data[split_at:], max_order)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", type=Path, help="corpus de train (o corpus completo si no se indica --holdout)")
    parser.add_argument("--holdout", type=Path, help="archivo independiente de evaluación")
    parser.add_argument("--split", type=float, default=0.8)
    parser.add_argument("--max-order", type=int, default=4)
    args = parser.parse_args()

    train_or_corpus = args.corpus.read_bytes()
    if args.holdout:
        test = args.holdout.read_bytes()
        train = train_or_corpus
        scores = witten_bell_holdout(train, test, args.max_order)
        print(f"train_bytes={len(train)} holdout_bytes={len(test)}")
    else:
        scores = witten_bell_scores(train_or_corpus, args.split, args.max_order)
        split_at = int(len(train_or_corpus) * args.split)
        print(
            f"bytes={len(train_or_corpus)} train={split_at} "
            f"test={len(train_or_corpus) - split_at} split={args.split:.3f}"
        )
    for order, score in enumerate(scores, start=1):
        print(f"Witten-Bell order={order}: {score:.4f} BPC")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
