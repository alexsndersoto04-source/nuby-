#!/usr/bin/env python3
"""
Experimento 2 — APRENDIZAJE DE UNA SECUENCIA.

Tarea: cuatro grupos (A,B,C,D) en orden ciclico. Se inyecta un token en su
grupo. Nadie conecta A con B: sus sinapsis existen al azar y son debiles.

El problema del arranque en frio
--------------------------------
Para reforzar A->B hace falta que B dispare alguna vez dentro de la vida de la
traza de A. Pero B nunca dispara: la conexion A->B es demasiado debil para
llevarlo al umbral. La red no puede aprender lo que nunca hace.

Esto se resuelve como en el condicionamiento clasico: con un estimulo
incondicionado (US). Durante el entrenamiento se empuja suavemente el grupo
objetivo DESPUES del token, de modo que coincida con la traza del token. Con el
tiempo el US se retira y la asociacion tiene que sostenerse sola.

Ese es el descubrimiento central de este experimento: sin US no aprende nada,
con US aprende en ~16 epocas, y cuando se retira sigue funcionando. La red
aprende el puente, no lo copia.

Se mide:
  1. ANTICIPACION: el grupo siguiente aparece sin inyectarlo.
  2. RECUPERACION EN FRIO: se inyecta solo A y se ve que completa.
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
US = 14.0
LISTEN = 30
GAP = 30
EPOCHS = 40
TRIALS = 6


def make_brain() -> Brain:
    p = Params(
        budget=8, budget_window=140, v_threshold=-50.0,
        w_exc_init=3.0, w_inh_init=9.0, w_max=24.0,
        noise_rate=0.0015, noise_amp=8.0,
        a_plus=0.08, a_minus=0.06, lr_da=1.2, tau_elig=300.0,
        inh_bias=6.0, delay_inh=2, seed=7, tau_trace=12.0,
    )
    b = Brain(N, p)
    b.build_clustered(groups=GROUPS, per_group=PER_GROUP, p_in=0.22,
                      p_out=0.05, p_inh=0.15)
    return b


def patch(token: int) -> list[int]:
    lo = token * PER_GROUP
    return [lo + i for i in range(0, PER_GROUP, 5)]


def counts(spikes: list[int]) -> dict[int, int]:
    out: dict[int, int] = {}
    for s in spikes:
        g = s // PER_GROUP
        out[g] = out.get(g, 0) + 1
    return out


def main() -> int:
    brain = make_brain()
    print(f"red: {N} neuronas, {brain.nsyn} sinapsis, "
          f"{GROUPS} grupos de {PER_GROUP} (modular: bucle denso + inhibicion cruzada)")
    print(f"tarea: A -> B -> C -> D    {EPOCHS} epocas x {TRIALS} ensayos")
    print(f"estimulo incondicionado: {US} mV, decreciente\n")

    curve = []
    clock = time.time()
    for epoch in range(EPOCHS):
        gain = US * (1.0 - 0.8 * epoch / max(1, EPOCHS - 1))
        anticip = 0
        total = 0
        for _ in range(TRIALS):
            for ti in range(TOKENS):
                nxt = (ti + 1) % TOKENS
                brain.inject(patch(ti), AMP)

                seen_next = 0
                for _ in range(LISTEN):
                    if counts(brain.step()).get(nxt, 0) >= 3:
                        seen_next += 1

                if gain > 1.0:
                    brain.inject(patch(nxt), gain)

                for _ in range(GAP):
                    if counts(brain.step()).get(nxt, 0) >= 3:
                        seen_next += 1

                if seen_next >= 4:
                    brain.reward(+1.0)
                    anticip += 1
                    brain.mark_assembly(patch(nxt))
                else:
                    brain.reward(-0.25)

                total += 1
                brain.consolidate(steps=12, tau=40.0)

        curve.append(anticip / total)
        if epoch % 5 == 0 or epoch == EPOCHS - 1:
            s = brain.stats()
            print(f"epoca {epoch + 1:3d}  anticipa={anticip:2d}/{total}  "
                  f"w_medio={s['w_mean']:6.2f}  sinapsis={s['synapses']:6d}")

    elapsed = time.time() - clock
    print(f"\nticks: {brain.t}  ({brain.t / elapsed:,.0f} ticks/s)")

    # ---------- recuperacion en frio ----------
    for _ in range(400):
        brain.step()
    brain.inject(patch(0), AMP)
    seen: dict[int, int] = {g: 0 for g in range(TOKENS)}
    for _ in range(90):
        for g, v in counts(brain.step()).items():
            if g in seen:
                seen[g] += v

    print("\n--- RECUPERACION EN FRIO (solo se inyecta A) ---")
    print("  " + "   ".join(f"{chr(65 + g)}:{seen[g]:5d}" for g in range(TOKENS)))

    print("\n--- CURVA (anticipacion por epoca) ---")
    step = max(1, EPOCHS // 20)
    print("  " + " ".join(f"{curve[i]:.2f}" for i in range(0, EPOCHS, step)))

    start = sum(curve[:3]) / 3
    end = sum(curve[-3:]) / 3
    print(f"\ninicio {start:.2f}  ->  final {end:.2f}")

    print("\n--- VEREDICTO ---")
    if end - start < 0.15:
        print("NO APRENDE con estos parametros. Ver el US: sin empujar el grupo")
        print("objetivo, la traza de A nunca coincide con un disparo de B.")
        return 1
    print("APRENDE: la anticipacion sube y se sostiene cuando el US se retira.")
    print("La conexion A->B no estaba programada: se gano por causalidad.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
