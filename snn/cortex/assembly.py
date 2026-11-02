"""
Asambleas y lectura.

Este archivo contiene las dos piezas que NO son parte del motor, y por eso
estan separadas: la deteccion de coaliciones y el readout.

La distincion importa:

  - El motor no sabe que es un simbolo.
  - El detector solo agrupa conjuntos de neuronas que disparan juntas.
  - El readout asocia esos conjuntos a los tokens que estaban presentes.

Ninguna de las tres dice "la neurona 57 significa 'a'". La etiqueta es una
estadistica sobre la coalicion, no una propiedad de la neurona. Si el detector
se apaga, la red sigue igual; si la red cambia, la etiqueta se muda sola.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from collections import Counter


@dataclass
class Assembly:
    members: set[int]
    fires: int = 0
    first_t: int = -1
    last_t: int = -1
    label: str | None = None

    def overlap(self, other: set[int]) -> float:
        if not self.members or not other:
            return 0.0
        inter = len(self.members & other)
        return inter / len(self.members | other)


class AssemblyDetector:
    """Detecta coaliciones por coincidencia temporal.

    Regla: en una ventana de `window` ticks, si dispararon al menos `min_size`
    neuronas, ese conjunto es un candidato. Si se parece a una asamblea ya
    conocida (Jaccard >= `merge_thr`), refuerza esa; si no, abre una nueva.

    Una asamblea nueva nace fragil: si en `warmup` ventanas no repite, se
    descarta. Es el filtro que separa estructura de ruido.
    """

    def __init__(self, window: int = 18, min_size: int = 4,
                 merge_thr: float = 0.55, warmup: int = 3):
        self.window = window
        self.min_size = min_size
        self.merge_thr = merge_thr
        self.warmup = warmup
        self.assemblies: list[Assembly] = []
        self._current: set[int] = set()
        self._t0 = 0
        self.events = 0
        self.rejected = 0

    def observe(self, t: int, spikes: list[int]) -> Assembly | None:
        """Se llama en cada tick con la lista de neuronas que dispararon."""
        self._current.update(spikes)
        if t - self._t0 < self.window:
            return None
        self._t0 = t
        pattern = self._current
        self._current = set()

        if len(pattern) < self.min_size:
            return None

        self.events += 1
        best, best_ov = None, 0.0
        for a in self.assemblies:
            ov = a.overlap(pattern)
            if ov > best_ov:
                best, best_ov = a, ov

        if best is not None and best_ov >= self.merge_thr:
            # consolidar: los miembros compartidos quedan, lo nuevo se suma si
            # el solapamiento fue alto (la asamblea crece hacia donde dispara)
            best.members |= pattern
            best.fires += 1
            best.last_t = t
            return best

        a = Assembly(members=set(pattern), fires=1, first_t=t, last_t=t)
        self.assemblies.append(a)
        return a

    def housekeeping(self) -> int:
        """Descarta asambleas que no repitieron. Devuelve cuantas se fueron."""
        before = len(self.assemblies)
        self.assemblies = [a for a in self.assemblies if a.fires >= self.warmup]
        gone = before - len(self.assemblies)
        self.rejected += gone
        return gone

    def match(self, pattern: set[int], thr: float = 0.4) -> Assembly | None:
        best, best_ov = None, 0.0
        for a in self.assemblies:
            ov = a.overlap(pattern)
            if ov > best_ov:
                best, best_ov = a, ov
        return best if best_ov >= thr else None

    def stability(self) -> float:
        """Cuanto se repiten. Alto = coaliciones solidas, bajo = ruido."""
        if not self.assemblies:
            return 0.0
        fires = [a.fires for a in self.assemblies]
        return sum(fires) / len(fires) / max(1, self.events) * len(self.assemblies)

    def summary(self) -> dict:
        return {
            "assemblies": len(self.assemblies),
            "events": self.events,
            "rejected": self.rejected,
            "mean_fires": round(sum(a.fires for a in self.assemblies) / max(1, len(self.assemblies)), 2),
            "mean_size": round(sum(len(a.members) for a in self.assemblies) / max(1, len(self.assemblies)), 1),
        }


class Readout:
    """Traduce asamblea -> simbolo por votacion, sin gradientes.

    No hay backprop: se cuenta cuantas veces cada asamblea aparecio mientras
    cierto token estaba presente. La etiqueta es la moda. Si dos asambleas se
    disputan el mismo simbolo, se resuelve por evidencia, no por peso.
    """

    def __init__(self):
        self.votes: dict[int, Counter] = {}

    def key(self, a: Assembly) -> int:
        return id(a)

    def teach(self, a: Assembly | None, label: str) -> None:
        if a is None:
            return
        self.votes.setdefault(self.key(a), Counter())[label] += 1

    def label_of(self, a: Assembly | None) -> str | None:
        if a is None:
            return None
        c = self.votes.get(self.key(a))
        if not c:
            return None
        return c.most_common(1)[0][0]

    def confidence(self, a: Assembly | None) -> float:
        if a is None:
            return 0.0
        c = self.votes.get(self.key(a))
        if not c:
            return 0.0
        total = sum(c.values())
        return c.most_common(1)[0][1] / total if total else 0.0

    def ambiguous(self) -> int:
        return sum(1 for c in self.votes.values() if len(c) > 1)
