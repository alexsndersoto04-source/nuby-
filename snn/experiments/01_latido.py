#!/usr/bin/env python3
"""
Experimento 1 — LATIDO Y ENRUTADO.

No se entrena nada. Se comprueba que la red existe y que sirve para algo.

La prueba es el ENRUTADO: se inyecta un parche en un grupo y se mira que grupo
domina la respuesta. Si el grupo dominante coincide con el inyectado, la red
tiene competicion funcional: un patron se impone y apaga a los demas. Si todos
los grupos disparan por igual, no hay red: hay una avalancha.

Tres cosas se miden, y las tres importan:

  1. LINEA BASE: actividad sin ningun estimulo. Si la red ya esta ruidosa de
     fabrica, ninguna senal se va a distinguir del ruido.
  2. CONCENTRACION: fraccion de los disparos que caen en el grupo dominante.
     1.0 = una asamblea limpia.  1/GROUPS = ruido difuso.
  3. ACIERTO: cuantas veces el grupo dominante es el que se estimulo.
"""

import os
import sys
import time
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from cortex import Brain, Params  # noqa: E402

N = 800
GROUPS = 10
PER_GROUP = 80


def make_brain() -> Brain:
    p = Params(
        budget=6, budget_window=120, v_threshold=-50.0,
        w_exc_init=4.5, w_inh_init=9.0,
        noise_rate=0.0015, noise_amp=8.0,
        inh_bias=6.0, delay_inh=2, seed=7,
    )
    b = Brain(N, p)
    b.build_clustered(groups=GROUPS, per_group=PER_GROUP,
                      p_in=0.22, p_out=0.02, p_inh=0.15)
    return b


def main() -> int:
    brain = make_brain()
    print(f"red: {N} neuronas, {brain.nsyn} sinapsis, "
          f"{GROUPS} grupos de {PER_GROUP}")
    print(f"conectividad: 22 % dentro del grupo, 2 % entre grupos, "
          f"15 % interneuronas\n")

    # ---------- 1. linea base ----------
    clock = time.time()
    for _ in range(300):
        brain.step()
    base_hit = Counter()
    for _ in range(300):
        base_hit.update(brain.step())
    base_spikes = sum(base_hit.values()) / 300
    base_part = len(base_hit) / N
    print(f"linea base (sin estimulo): {base_spikes:5.2f} disparos/tick  "
          f"{base_part * 100:5.1f} % de la red involucrada")

    # ---------- 2. enrutado ----------
    centers = [g * PER_GROUP + PER_GROUP // 2 for g in range(GROUPS)]
    doms, concs, responses = [], [], []
    for g, c in enumerate(centers):
        pat = [(c + d) % N for d in range(-8, 9, 2)]
        brain.inject(pat, 20.0)
        hits = Counter()
        group_hits = Counter()
        fracs = []
        for _ in range(40):
            sp = brain.step()
            hits.update(sp)
            for s in sp:
                group_hits[s // PER_GROUP] += 1
            if sp:
                fracs.append(brain.group_fraction(sp))
        for _ in range(120):
            brain.step()
        doms.append(group_hits.most_common(1)[0][0] if group_hits else -1)
        concs.append(sum(fracs) / len(fracs) if fracs else 0.0)
        responses.append(sum(hits.values()))

    correct = sum(1 for g, d in enumerate(doms) if d == g) / GROUPS
    mean_conc = sum(concs) / len(concs)
    mean_resp = sum(responses) / len(responses)
    elapsed = time.time() - clock

    print(f"respuesta a un parche ...... {mean_resp:5.1f} disparos")
    print(f"concentracion en un grupo .. {mean_conc:.3f}   (1.0 = asamblea limpia, "
          f"{1 / GROUPS:.2f} = ruido)")
    print(f"acierto de enrutado ........ {correct * 100:.1f} %")
    print(f"velocidad .................. {brain.t / elapsed:,.0f} ticks/s")

    # ---------- 3. veredicto ----------
    print("\n--- VEREDICTO ---")
    ok = True
    if base_spikes > 3.0:
        print("RUIDOSA: la red dispara sola sin estimulo. Ninguna senal se distingue.")
        ok = False
    if mean_conc < 0.6:
        print("AVALANCHA: el estimulo enciende toda la red. Falta inhibicion cruzada.")
        ok = False
    if correct < 0.8:
        print("NO ENRUTA: el grupo dominante no depende del estimulo.")
        ok = False
    if ok:
        print("VIVA Y SELECTIVA.")
        print(f"Cada estimulo enciende uno de {GROUPS} grupos y apaga el resto.")
        print("Eso es competicion funcional: la base de una asamblea celular.")
        print("Ahora puede aprenderse algo encima. Ver 02_aprendizaje.py.")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
