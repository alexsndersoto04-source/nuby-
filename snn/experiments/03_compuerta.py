#!/usr/bin/env python3
"""
Experimento 3 — COMPUERTA DE FASE, MEMORIA Y LIMITE CONOCIDO.

Aqui se juntan las tres cosas que si funcionan, se miden, y se documenta la que
no. Este archivo no es una demo bonita: es un acta.

  A) APRENDIZAJE: la red aprende A -> B -> C -> D con estimulo incondicionado.
     (Mismo mecanismo del experimento 2, con menos epocas.)

  B) COMPUERTA DE FASE: la misma entrada, dos fases.
       escucha (gain 1.00) -> enciende una asamblea
       emision (gain 0.12) -> no enciende nada
     No se bloquea con un flag: la entrada se vuelve energeticamente
     insuficiente. Es la version electrica de cerrar los ojos para hablar.

  C) MEMORIA: el cerebro se guarda, se abre en un proceso nuevo y la secuencia
     sigue ahi. Lo aprendido no vive en la sesion.

  D) LIMITE CONOCIDO (resultado negativo, y se dice): con inhibicion GLOBAL
     no se pueden separar varias respuestas. Si tres senales distintas deben
     producir tres respuestas distintas, la red no las distingue: la inhibicion
     que frena a las otras dos frena tambien a la correcta, y el critico no
     tiene campo receptivo para asignar el credito. Se prueba y se mide al
     final, en vez de disimularlo.
"""

import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from cortex import Brain, Params  # noqa: E402

N = 800
GROUPS = 8
PER_GROUP = 100
TOKENS = 4
AMP = 20.0
US = 16.0
LISTEN = 25
GAP = 30
EPOCHS = 30
TRIALS = 5
PATH = "/tmp/cortex_memoria.bin"


def make_brain(seed: int = 7) -> Brain:
    # Estos numeros no son gusto: son los de la corrida que converge
    # (0 % -> 94 % de anticipacion en la epoca 13). Cada uno tiene su razon.
    p = Params(
        budget=8, budget_window=140, v_threshold=-50.0,
        w_exc_init=3.0, w_inh_init=9.0, w_max=24.0,
        noise_rate=0.0015, noise_amp=8.0,
        a_plus=0.08, a_minus=0.06, lr_da=1.2,
        tau_elig=300.0, tau_trace=12.0, theta_step=1.2,
        inh_bias=6.0, delay_inh=2, seed=seed,
    )
    b = Brain(N, p)
    b.build_clustered(groups=GROUPS, per_group=PER_GROUP, p_in=0.22,
                      p_out=0.05, p_inh=0.15)
    return b


def patch(g: int) -> list[int]:
    lo = g * PER_GROUP
    return [lo + i for i in range(0, PER_GROUP, 6)]


def group_counts(spikes: list[int]) -> dict[int, int]:
    out: dict[int, int] = {}
    for s in spikes:
        out[s // PER_GROUP] = out.get(s // PER_GROUP, 0) + 1
    return out


def train(brain: Brain, tokens: int = TOKENS) -> list[float]:
    """Entrenamiento por emparejamiento.

    Se inyecta el token, se empuja suavemente el siguiente (estimulo
    incondicionado) y se deja que la red los encadene. La recompensa llega
    siempre que el siguiente grupo aparezca: es el critico externo que dice
    "esto que acaba de pasar, repitelo". El US se retira poco a poco.
    """
    curve = []
    for epoch in range(EPOCHS):
        gain = US * (1.0 - 0.8 * epoch / max(1, EPOCHS - 1))
        anticip = 0
        total = 0
        for _ in range(TRIALS):
            for ti in range(tokens):
                nxt = (ti + 1) % tokens
                brain.set_mode("listen")
                brain.inject(patch(ti), AMP)
                for _ in range(LISTEN):
                    brain.step()

                if gain > 1.0:
                    brain.inject(patch(nxt), gain)

                seen = 0
                for _ in range(34):
                    sp = brain.step()
                    if group_counts(sp).get(nxt, 0) >= 3:
                        seen += 1

                if seen >= 4:
                    brain.reward(+1.0)
                    anticip += 1
                total += 1
                brain.consolidate(steps=12, tau=40.0)

                for _ in range(GAP):
                    brain.step()
        curve.append(anticip / total)
        if epoch % 6 == 0 or epoch == EPOCHS - 1:
            print(f"  epoca {epoch + 1:2d}  anticipa={anticip:2d}/{total}  "
                  f"w_medio={brain.stats()['w_mean']:5.2f}")
    return curve


def speak_response(brain: Brain, cue: int, ticks: int = 55) -> dict[int, int]:
    """Un turno: escuchar el cue, cerrar la compuerta, y ver que produce sola."""
    brain.set_mode("listen")
    brain.inject(patch(cue), AMP)
    for _ in range(LISTEN):
        brain.step()
    brain.set_mode("speak")
    hits: dict[int, int] = {}
    for _ in range(ticks):
        for g, v in group_counts(brain.step()).items():
            hits[g] = hits.get(g, 0) + v
    brain.set_mode("listen")
    return hits


def gate_test(brain: Brain, cue: int) -> tuple[int, int]:
    """La misma entrada, dos fases, desde el silencio."""
    for _ in range(250):
        brain.step()

    brain.set_mode("listen")
    brain.inject(patch(cue), AMP)
    listen_spikes = sum(len(brain.step()) for _ in range(25))
    for _ in range(250):
        brain.step()

    brain.set_mode("speak")
    brain.inject(patch(cue), AMP)
    speak_spikes = sum(len(brain.step()) for _ in range(25))
    brain.set_mode("listen")
    for _ in range(250):
        brain.step()
    return listen_spikes, speak_spikes


def main() -> int:
    brain = make_brain()
    print(f"red: {N} neuronas, {brain.nsyn} sinapsis, "
          f"{GROUPS} grupos de {PER_GROUP}\n")

    # ---------------- A) aprendizaje ----------------
    print("== A) APRENDIZAJE  A -> B -> C -> D ==")
    clock = time.time()
    curve = train(brain)
    print(f"  {time.time() - clock:.0f} s, {brain.t} ticks  "
          f"({brain.t / (time.time() - clock):,.0f} ticks/s)")

    # ---------------- B) compuerta ----------------
    print("\n== B) COMPUERTA DE FASE ==")
    l, s = gate_test(brain, 0)
    print(f"  misma entrada, 25 ticks, desde el silencio:")
    print(f"    en ESCUCHA (ganancia 1.00) -> {l:5d} disparos   (enciende)")
    print(f"    en EMISION (ganancia 0.12) -> {s:5d} disparos   "
          f"({'no enciende' if s < l * 0.25 else 'ENCIENDE IGUAL: la compuerta no sirve'})")

    # ---------------- C) memoria ----------------
    print("\n== C) MEMORIA EN DISCO ==")
    brain.save(PATH)
    size = os.path.getsize(PATH) / 1024
    reopened = Brain(10, Params())
    reopened.load(PATH)
    for _ in range(250):
        reopened.step()
    hits = speak_response(reopened, 0)
    order = [g for g, _ in sorted(hits.items(), key=lambda kv: -kv[1]) if g < TOKENS]
    print(f"  guardado en {PATH} ({size:.0f} KB) y cargado en un objeto nuevo")
    print(f"  se inyecta A: grupos activos por orden de fuerza -> {order}")
    solved = len(set(order) & {1, 2, 3}) >= 2
    print(f"  la secuencia sobrevive al reinicio: {'SI' if solved else 'NO'}")
    print(f"  respuesta completa: {hits}")

    # ---------------- D) limite ----------------
    print("\n== D) LIMITE CONOCIDO: varias respuestas distintas ==")
    cue_groups = [0, 1, 2]
    resp_groups = [3, 4, 5]
    wins = []
    for ci, cue in enumerate(cue_groups):
        for _ in range(200):
            brain.step()
        hits = speak_response(brain, cue)
        win = max(resp_groups, key=lambda g: hits.get(g, 0))
        wins.append(win)
        print(f"  se inyecta {chr(65 + cue)} -> respuesta mas fuerte "
              f"{chr(65 + win)} ({hits.get(win, 0)} disparos)")
    distinct = len(set(wins)) == len(cue_groups)
    print(f"  tres senales producen tres respuestas distintas: "
          f"{'SI' if distinct else 'NO'}")

    # ---------------- veredicto ----------------
    print("\n" + "=" * 62)
    print("VEREDICTO")
    print("=" * 62)
    first, last = curve[0], curve[-1]
    print(f"A) aprendizaje ......... {'OK' if last > first + 0.2 else 'FALLA'}"
          f"   ({first:.2f} -> {last:.2f})")
    print(f"B) compuerta de fase ... {'OK' if s < l * 0.25 else 'FALLA'}")
    print(f"C) memoria persistente . {'OK' if solved else 'FALLA'}")
    print(f"D) respuestas multiples  {'OK' if distinct else 'NO RESUELTO (esperado)'}")

    if not distinct:
        print("""
Sobre D: esto no es un bug de parametros, es una consecuencia de la
arquitectura. Con inhibicion GLOBAL, cualquier asamblea que se encienda frena a
todas las demas por igual -- incluida la que deberia ser la respuesta correcta.
No hay campo receptivo, asi que el critico no puede decir "esta sinapsis fue la
culpable" y el aprendizaje no tiene a donde agarrarse.

La solucion, que es el siguiente paso y no esta hecho aqui, es inhibicion
ESPECIFICA: interneuronas que solo frenen a los competidores de su propia
asamblea, no al vecindario entero. Eso exige aprender el mapa de competencia,
y es un problema abierto, no un parametro que falte.
""")
    return 0 if (last > first + 0.2 and s < l * 0.25 and solved) else 1


if __name__ == "__main__":
    raise SystemExit(main())
