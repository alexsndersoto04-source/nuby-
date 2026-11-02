"""
CORTEX — motor de red neuronal de pulsos con lexico emergente.

Sin numpy. Sin backpropagation. Sin matrices densas.

Principios, en orden de importancia:

1. NO EXISTE DICCIONARIO.  Ninguna neurona "significa" nada. El simbolo es una
   coalicion de neuronas que se estabilizo por competicion. El decoder aprende a
   nombrar la coalicion despues, mirando quien disparo.

2. EL LEXICO ES UNA SALIDA, NO UNA ENTRADA.  La capa sensorial es un hash
   disperso determinista: el mismo token siempre inyecta energia en el mismo
   conjunto de neuronas, pero ese conjunto no es especial hasta que la
   plasticidad lo convierte en asamblea.

3. LA COMPUERTA ES TEMPORAL.  Una sola poblacion. Entre fases, la MISMA
   poblacion cambia de polaridad: en escucha el reposo es bajo y se deja
   reclutar; en emision el reposo sube y el estimulo externo ya no la alcanza
   sin empujar mucho. La entrada no se apaga con un flag: se vuelve cara.

4. LA HOMEOSTASIS ES UN PRESUPUESTO.  Cada neurona tiene un cupo de disparos por
   ventana. Al agotarlo su umbral se dispara y no puede disparar mas hasta que
   la ventana se renueve. Eso reparte la representacion y evita que tres
   neuronas secuestren la red.

5. EL TIEMPO ES VIRTUAL.  Simular 1 ms no cuesta 1 ms. Se procesa lo mas rapido
   que la maquina permita sobre una cola de eventos indexada por instante de
   llegada. La red no sabe que existe el reloj de pared.

Unidades: tiempo entero en pasos ("ticks", nominalmente 1 ms). Reproducible con
semilla: misma semilla + misma secuencia de estimulos = mismo resultado.
"""

from __future__ import annotations

import math
import random
import json
from array import array
from dataclasses import dataclass, asdict
from typing import Iterable


# ----------------------------------------------------------------------------
# Parametros
# ----------------------------------------------------------------------------

@dataclass
class Params:
    # --- dinamica de membrana (LIF) ---
    v_rest: float = -65.0
    v_reset: float = -70.0
    v_threshold: float = -52.0
    tau_m: float = 20.0
    refractory: int = 2

    # --- sinapsis ---
    w_exc_init: float = 6.0
    w_inh_init: float = 10.0
    w_max: float = 24.0
    k_target: float = 10.0      # sinapsis entrantes efectivas que una neurona defiende
    delay_exc: int = 1
    delay_inh: int = 3

    # --- homeostasis por presupuesto ---
    budget: int = 7
    budget_window: int = 100
    theta_step: float = 1.2
    theta_tau: float = 400.0

    # --- plasticidad ---
    tau_trace: float = 12.0
    tau_elig: float = 220.0
    a_plus: float = 0.35
    a_minus: float = 0.25
    lr_da: float = 3.0
    elig_cap: float = 2.0

    # --- dopamina ---
    tau_da: float = 180.0

    # --- ruido de fondo (actividad espontanea) ---
    noise_rate: float = 0.004     # fraccion de neuronas que recibe corrientes por tick
    noise_amp: float = 9.0
    inh_bias: float = 7.0         # las interneuronas son mas excitables

    # --- compuerta de fase ---
    v_rest_listen: float = -65.0
    v_rest_speak: float = -46.0

    # --- reclutamiento ---
    recruit_every: int = 25
    seed: int = 1234


# ----------------------------------------------------------------------------
# Cola de eventos indexada por instante de llegada
# ----------------------------------------------------------------------------

EV_ARRIVE = 0


class BucketQueue:
    """Buffer circular de buckets. En cada tick se vacia SOLO el bucket de ese
    tick: coste O(eventos entregados), no O(neuronas) ni O(sinapsis).

    Cada evento: (t, pre, flags, post, sid)
    """

    __slots__ = ("head", "mask", "buffers")

    def __init__(self, span: int = 256):
        span = 1 << max(2, (span - 1).bit_length())
        self.mask = span - 1
        self.head = 0
        self.buffers: list[list[tuple]] = [[] for _ in range(span)]

    @property
    def span(self) -> int:
        return self.mask + 1

    def push(self, at: int, pre: int, flags: int, post: int, sid: int) -> None:
        if at <= self.head:
            # llego tarde: se entrega en este mismo tick
            at = self.head + 1
        if at - self.head > self.mask:
            self._grow(at)
        self.buffers[at & self.mask].append((at, pre, flags, post, sid))

    def _grow(self, at: int) -> None:
        span = self.span
        while at - self.head > span - 1:
            span <<= 1
        flat: list[tuple] = []
        for b in self.buffers:
            flat.extend(b)
        self.buffers = [[] for _ in range(span)]
        self.mask = span - 1
        for ev in flat:
            self.buffers[ev[0] & self.mask].append(ev)

    def drain(self) -> list[tuple]:
        t = self.head
        b = self.buffers[t & self.mask]
        self.buffers[t & self.mask] = []
        self.head = t + 1
        return b


# ----------------------------------------------------------------------------
# Cerebro: estado en arrays planos (SoA), sinapsis en CSR
# ----------------------------------------------------------------------------

class Brain:
    def __init__(self, n: int, params: Params | None = None):
        p = self.p = params or Params()
        self.rng = random.Random(p.seed)
        self.n = n
        self.t = 0

        # --- neuronas ---
        self.v = array("d", [p.v_rest] * n)
        self.theta = array("d", [0.0] * n)
        self.post_trace = array("d", [0.0] * n)   # traza de disparo (para STDP)
        self.last_spike = array("i", [-100000] * n)
        self.refract = array("i", [0] * n)
        self.budget_spent = array("i", [0] * n)
        self.budget_start = array("i", [0] * n)
        self.spike_count = array("i", [0] * n)
        self.is_inh = bytearray(n)
        self.is_sensory = bytearray(n)

        # --- sinapsis, CSR por neurona presinaptica ---
        self.offsets = array("i", [0] * (n + 1))
        self.dst = array("i")
        self.w = array("d")
        self.inh = bytearray()
        self.dly = array("b")
        self.elig = array("d")
        self.nsyn = 0

        # --- CSR inverso (por neurona post), para STDP en el disparo ---
        self.inc = array("i", [0] * (n + 1))
        self.inc_src = array("i")
        self.inc_sid = array("i")

        self.da = 0.0
        self.gain = 1.0
        self.phase = "listen"
        self.queue = BucketQueue(256)

        # --- poblacion activa: solo se procesa lo que puede cambiar ---
        self._active: set[int] = set()
        self._active_next: set[int] = set()
        self._dirty_syn: set[int] = set()

        # --- reclutamiento ---
        self.free_neurons = list(range(n))
        self.rng.shuffle(self.free_neurons)
        self.recruited = 0
        self._pending_assembly: list[int] = []

        # --- constantes de decaimiento precalculadas ---
        self._dec_v = math.exp(-1.0 / p.tau_m)
        self._dec_trace = math.exp(-1.0 / p.tau_trace)
        self._dec_elig = math.exp(-1.0 / p.tau_elig)
        self._dec_theta = math.exp(-1.0 / p.theta_tau)
        self._dec_da = math.exp(-1.0 / p.tau_da)

        # --- estadisticas ---
        self.steps = 0
        self.total_spikes = 0
        self.spikes_this_tick: list[int] = []

    # ---------------- construccion del grafo ----------------

    def random_graph(self, k: int = 8, p_inh: float = 0.20, block: int = 20,
                     k_inh: int = 30, inh_span: int = 90) -> None:
        """Grafo disperso CON topografia y CON inhibicion divergente.

        Dos asimetrias deliberadas, y ambas son la razon de que esto funcione:

        1. Excitacion LOCAL (`block`): cada neurona excita a sus vecinas de
           indice. Sin esto la senal muere en el primer salto porque dos
           neuronas que reciben del mismo grupo nunca coinciden.

        2. Inhibicion ANCHA (`inh_span`) y de abanico grande (`k_inh`): una
           interneurona no inhibe a su vecina, inhibe a una region entera. Esa
           asimetria --excitacion corta, inhibicion larga-- es lo que produce
           ganador-toma-todo: el grupo que mas disparo apaga al resto en lugar
           de que todo se difumine en un unico blob de actividad.
        """
        n = self.n
        n_inh = int(n * p_inh)
        inh_ids = set(self.rng.sample(range(n), n_inh))
        for i in range(n):
            self.is_inh[i] = 1 if i in inh_ids else 0

        rows: list[list[int]] = [[] for _ in range(n)]
        half = max(1, block // 2)
        for pre in range(n):
            if self.is_inh[pre]:
                for _ in range(k_inh):
                    post = (pre + self.rng.randint(-inh_span, inh_span)) % n
                    if post != pre:
                        rows[pre].append(post)
                # y vecinas cercanas, para no dejar el entorno sin freno
                for _ in range(3):
                    post = (pre + self.rng.randint(-half, half)) % n
                    if post != pre:
                        rows[pre].append(post)
            else:
                for _ in range(k):
                    post = (pre + self.rng.randint(-half, half)) % n
                    if post != pre:
                        rows[pre].append(post)

        p = self.p
        off = array("i", [0])
        dst, w, inh, dly, elig = array("i"), array("d"), bytearray(), array("b"), array("d")
        for pre in range(n):
            for post in rows[pre]:
                dst.append(post)
                if self.is_inh[pre]:
                    w.append(p.w_inh_init)
                    inh.append(1)
                    dly.append(p.delay_inh)
                else:
                    w.append(p.w_exc_init)
                    inh.append(0)
                    dly.append(p.delay_exc)
                elig.append(0.0)
            off.append(len(dst))
        self.offsets, self.dst, self.w = off, dst, w
        self.inh, self.dly, self.elig = inh, dly, elig
        self.nsyn = len(dst)
        self.rebuild_incoming()

    def build_clustered(self, groups: int = 10, per_group: int = 60,
                        p_in: float = 0.25, p_out: float = 0.02,
                        p_inh: float = 0.15, p_inh_in: float = 0.35,
                        p_inh_out: float = 0.02) -> None:
        """Red modular: grupos con bucle recurrente denso y competicion entre
        grupos.

        Esta es la unica topologia de las probadas que produce asambleas de
        verdad, y la razon es estructural, no de ajuste:

        - DENTRO del grupo la conectividad es alta (p_in). Eso crea un bucle
          excitatorio capaz de sostenerse solo: el grupo es BISTABLE. Una vez
          encendido sigue encendido sin que nadie lo empuje. Eso es memoria.

        - ENTRE grupos la conectividad es baja (p_out), pero las interneuronas
          proyectan ancho (p_inh_out). Eso crea competicion: el grupo que se
          enciende inhibe a los demas y les impide encenderse. Eso es seleccion.

        - Las interneuronas DENTRO del grupo (p_inh_in) son el freno que evita
          que el bucle denso se convierta en epilepsia.

        Bucle denso + inhibicion interna + inhibicion cruzada = asamblea celular
        estable y exclusiva. Sin las tres, no hay asamblea: hay avalancha.
        """
        n = self.n
        if groups * per_group != n:
            raise ValueError(f"groups*per_group ({groups*per_group}) != n ({n})")
        self.groups = groups
        self.per_group = per_group
        self.group_of = [i // per_group for i in range(n)]

        inh_ids = set(self.rng.sample(range(n), int(n * p_inh)))
        for i in range(n):
            self.is_inh[i] = 1 if i in inh_ids else 0

        p = self.p
        off = array("i", [0])
        dst, w, inh, dly, elig = array("i"), array("d"), bytearray(), array("b"), array("d")

        for pre in range(n):
            g = pre // per_group
            lo = g * per_group
            hi = lo + per_group
            targets: list[int] = []
            if self.is_inh[pre]:
                k_in = max(1, int(per_group * p_inh_in))
                for _ in range(k_in):
                    targets.append(self.rng.randrange(lo, hi))
                k_out = max(0, int((n - per_group) * p_inh_out))
                for _ in range(k_out):
                    t = self.rng.randrange(n)
                    if not (lo <= t < hi):
                        targets.append(t)
            else:
                k_in = max(1, int(per_group * p_in))
                for _ in range(k_in):
                    targets.append(self.rng.randrange(lo, hi))
                k_out = max(0, int((n - per_group) * p_out))
                for _ in range(k_out):
                    t = self.rng.randrange(n)
                    if not (lo <= t < hi):
                        targets.append(t)

            for post in targets:
                if post == pre:
                    continue
                dst.append(post)
                if self.is_inh[pre]:
                    w.append(p.w_inh_init)
                    inh.append(1)
                    dly.append(p.delay_inh)
                else:
                    w.append(p.w_exc_init)
                    inh.append(0)
                    dly.append(p.delay_exc)
                elig.append(0.0)
            off.append(len(dst))

        self.offsets, self.dst, self.w = off, dst, w
        self.inh, self.dly, self.elig = inh, dly, elig
        self.nsyn = len(dst)
        self.free_neurons = [i for i in range(n)]
        self.rng.shuffle(self.free_neurons)
        self.rebuild_incoming()

    def rest_cycle(self, path: str = "/tmp/_cortex_rest.bin", prune: float = 0.0) -> int:
        """Ciclo de reposo: la forma barata y correcta de "dormir".

        No se borra nada aprendido. Se reconstruye el estado dinamico --los
        potenciales, las trazas y las colas de eventos-- dejando intacto lo
        unico que importa: la matriz de pesos.

        Es imprescindible y se comprobo empiricamente: sin este ciclo la
        plasticidad tiene un sesgo sistematico, porque la dopamina solo suma y
        la depresion solo resta, y el resultado es que los pesos derivan hasta
        que la red se enciende entera. Una red asi "acierta" todo, porque todo
        dispara siempre. Ese acierto es falso.

        Con el ciclo la red vuelve a estar callada despues de cada bloque y el
        aprendizaje, aunque mas lento, es real: lo que predice lo predice desde
        el silencio.

        Devuelve el numero de sinapsis que se podaron (0 si no se poda).
        """
        self.save(path)
        self.load(path)
        if prune > 0.0:
            return self.prune(min_w=prune)
        return 0

    def group_fraction(self, spikes: list[int]) -> float:
        """Que fraccion de la actividad se concentra en el grupo dominante.
        Cerca de 1 = asamblea limpia. Cerca de 1/groups = ruido difuso."""
        if not spikes:
            return 0.0
        buckets: dict[int, int] = {}
        for s in spikes:
            g = s // self.per_group
            buckets[g] = buckets.get(g, 0) + 1
        return max(buckets.values()) / len(spikes)

    def dominant_group(self, spikes: list[int]) -> int:
        if not spikes:
            return -1
        buckets: dict[int, int] = {}
        for s in spikes:
            g = s // self.per_group
            buckets[g] = buckets.get(g, 0) + 1
        return max(buckets.items(), key=lambda kv: kv[1])[0]

    def rebuild_incoming(self) -> None:
        n = self.n
        counts = array("i", [0] * n)
        for post in self.dst:
            counts[post] += 1
        inc = array("i", [0] * (n + 1))
        acc = 0
        for i in range(n):
            inc[i] = acc
            acc += counts[i]
        inc[n] = acc
        src = array("i", [0]) * acc
        sid = array("i", [0]) * acc
        cursor = array("i", inc[:-1])
        for s in range(self.nsyn):
            post = self.dst[s]
            pos = cursor[post]
            src[pos] = self._find_pre(s)
            sid[pos] = s
            cursor[post] = pos + 1
        self.inc, self.inc_src, self.inc_sid = inc, src, sid

    def _find_pre(self, sid: int) -> int:
        # busqueda binaria en offsets
        offs = self.offsets
        lo, hi = 0, self.n
        while lo < hi:
            mid = (lo + hi) // 2
            if offs[mid + 1] <= sid:
                lo = mid + 1
            else:
                hi = mid
        return lo

    # ---------------- codificacion ----------------

    def token_cells(self, token: int, width: int = 12) -> list[int]:
        """Hash disperso determinista: mismo token -> mismo sitio; tokens
        proximos no, pero eso es justo lo que la plasticidad debe arreglar."""
        rng = random.Random((self.p.seed * 1000003) ^ (token * 2654435761) ^ 0x9E3779B9)
        return [rng.randrange(self.n) for _ in range(width)]

    def token_patch(self, token: int, width: int = 8, spread: int = 4) -> list[int]:
        """Parche contiguo: el token enciende una zona compacta de la poblacion,
        no puntos sueltos. Coincide con la topografia del grafo, asi que la
        activacion se propaga en lugar de extinguirse."""
        rng = random.Random((self.p.seed * 1000003) ^ (token * 2654435761) ^ 0x85EBCA6B)
        center = rng.randrange(self.n)
        cells: list[int] = []
        for d in range(-width, width + 1):
            cells.append((center + d * spread) % self.n)
        return cells

    def mark_sensory(self, ids: Iterable[int]) -> None:
        for i in ids:
            self.is_sensory[i] = 1

    # ---------------- inyeccion ----------------

    def inject(self, cells: Iterable[int], amp: float = 18.0) -> None:
        """Un token no se 'representa': se inyecta como energia. La red decide.

        La amplitud pasa por la ganancia de la compuerta de fase: en emision la
        entrada llega atenuada. No se bloquea con un flag -- se vuelve cara.
        """
        if self.gain <= 0.0:
            return
        amp *= self.gain
        v = self.v
        act = self._active
        for c in cells:
            v[c] += amp
            act.add(c)

    def set_mode(self, mode: str) -> None:
        """Compuerta de fase por ganancia sensorial.

        "listen": la entrada entra a plena potencia (gain 1.0).
        "speak" : la entrada entra al 12 % -- sigue llegando, pero ya no basta
                  para encender una asamblea. La red solo puede seguir su propio
                  curso. Es la version electrica de cerrar los ojos para hablar.

        No se toca ningun peso ni ningun potencial: la poblacion es la misma y
        el grafo es el mismo. Solo cambia cuanto pesa el mundo.
        """
        if mode == "listen":
            self.gain = 1.0
            self.phase = "listen"
        elif mode == "speak":
            self.gain = 0.12
            self.phase = "speak"
        else:
            raise ValueError("modo desconocido: " + mode)

    # ---------------- reloj ----------------

    def step(self) -> list[int]:
        p = self.p
        t = self.t
        v = self.v
        w = self.w
        dst = self.dst
        elig = self.elig
        ptrace = self.post_trace
        refr = self.refract
        theta = self.theta
        thr = p.v_threshold
        dirty = self._dirty_syn

        # ---- Fase 1: llegadas ----
        for at, pre, _flags, post, sid in self.queue.drain():
            if self.inh[sid]:
                v[post] -= w[sid]
            else:
                v[post] += w[sid]
            self._active.add(post)

        # ---- Fase 1b: ruido de fondo ----
        # Una red LIF sin corriente espontanea esta muda: solo puede obedecer al
        # estimulo. Con un murmullo basal puede sostener reverberacion propia y
        # competir consigo misma, que es lo que se busca.
        if p.noise_rate > 0.0:
            n_noise = int(self.n * p.noise_rate)
            rnd = self.rng.random
            for _ in range(n_noise):
                j = int(rnd() * self.n)
                v[j] += p.noise_amp * (0.5 + rnd())
                self._active.add(j)

        # ---- Fase 2: integracion y disparo ----
        spikes: list[int] = []
        dec_v = self._dec_v
        v_rest = p.v_rest
        for i in tuple(self._active):
            if refr[i] > 0:
                refr[i] -= 1
                v[i] = p.v_reset
                self._active_next.add(i)
                continue

            if t - self.budget_start[i] >= p.budget_window:
                self.budget_start[i] = t
                self.budget_spent[i] = 0

            eff_thr = thr - (p.inh_bias if self.is_inh[i] else 0.0)
            if v[i] >= eff_thr + theta[i] and self.budget_spent[i] < p.budget:
                spikes.append(i)
                continue

            v[i] = p.v_reset + (v[i] - p.v_reset) * dec_v
            if v[i] < v_rest:
                v[i] = v_rest
            else:
                self._active_next.add(i)

        # ---- Fase 3: propagacion + STDP local ----
        offs = self.offsets
        dly = self.dly
        inc = self.inc
        inc_src = self.inc_src
        inc_sid = self.inc_sid
        q = self.queue
        a_plus = p.a_plus
        a_minus = p.a_minus

        for i in spikes:
            # ---------------------------------------------------------------
            # PLASTICIDAD DE TRES FACTORES (no STDP de pares)
            #
            # El STDP clasico solo asocia disparos separados por pocos ms. Aqui
            # la asociacion que hay que aprender cruza decenas de ticks, asi que
            # el emparejamiento se hace contra la TRAZA DE ELEGIBILIDAD, que vive
            # cientos de ms, y no contra el disparo en si:
            #
            #   al disparar i, cada sinapsis SALIENTE i->j queda marcada como
            #   "candidata a predecir lo que venga". No cambia el peso todavia.
            #
            #   si luego j dispara dentro de la vida de la traza Y llega
            #   dopamina, esa marca se convierte en peso. El peso se escribe en
            #   consolidate(), no aqui.
            #
            # Dos asimetrias, ambas evaluadas cuando i dispara:
            #   pre disparo hace poco  ->  potenciar (causalidad correcta)
            #   post disparo hace poco ->  depresion (causalidad invertida)
            # Y el tercer factor: sin dopamina, la marca se disipa sin dejar
            # rastro en el peso. Nada de esto modifica w directamente.
            # ---------------------------------------------------------------
            # 1) POTENCIACION. La neurona i acaba de disparar. Cada sinapsis
            #    entrante pre->i suma la traza de su presinaptica: si pre
            #    disparo HACE POCO, la causalidad es correcta y la sinapsis
            #    queda como candidata. Si pre lleva tiempo callada, su traza es
            #    cero y no pasa nada.
            for k in range(inc[i], inc[i + 1]):
                s = inc_sid[k]
                if self.inh[s]:
                    continue
                tr = ptrace[inc_src[k]]
                if tr > 0.0:
                    x = elig[s] + a_plus * tr
                    cap = p.elig_cap
                    elig[s] = cap if x > cap else x
                    dirty.add(s)

            # 2) DEPRESION. La misma neurona i tiene salientes i->j. Si j ya
            #    habia disparado hace poco, la causa va al reves: la sinapsis
            #    esta prediciendo algo que ya ocurrio. Se castiga.
            for s in range(offs[i], offs[i + 1]):
                if self.inh[s]:
                    continue
                tr = ptrace[dst[s]]
                if tr > 0.0:
                    x = elig[s] - a_minus * tr
                    cap = p.elig_cap
                    elig[s] = -cap if x < -cap else x
                    dirty.add(s)

            v[i] = p.v_reset
            refr[i] = p.refractory
            self.last_spike[i] = t
            self.budget_spent[i] += 1
            self.spike_count[i] += 1
            ptrace[i] = 1.0
            theta[i] += p.theta_step

            for s in range(offs[i], offs[i + 1]):
                j = dst[s]
                d = dly[s]
                q.push(t + (d if d > 0 else 1), i, EV_ARRIVE, j, s)
                self._active_next.add(j)

        # ---- Fase 4: relajacion de trazas ----
        dec_pt = self._dec_trace
        for i in tuple(self._active | (set(spikes))):
            ptrace_i = ptrace[i] * dec_pt
            ptrace[i] = ptrace_i if ptrace_i > 1e-4 else 0.0
        dec_e = self._dec_elig
        for s in dirty:
            e = elig[s] * dec_e
            elig[s] = e if abs(e) > 1e-6 else 0.0
        self.da *= self._dec_da
        dec_th = self._dec_theta
        for i in tuple(self._active):
            theta[i] *= dec_th

        # ---- Fase 5: reclutamiento ----
        if self.free_neurons and t % p.recruit_every == 0:
            self._recruit()

        self._active = self._active_next
        self._active_next = set()
        self.t += 1
        self.steps += 1
        self.spikes_this_tick = spikes
        self.total_spikes += len(spikes)
        return spikes

    def run(self, ticks: int) -> None:
        for _ in range(ticks):
            self.step()

    # ---------------- reclutamiento ----------------

    def _recruit(self) -> None:
        """Una neurona virgen recibe una sinapsis desde cada miembro de la
        asamblea que se marco. No se crean conexiones al azar a escala: se crean
        donde ya hay algo latiendo."""
        members = self._pending_assembly
        if not members or not self.free_neurons:
            return
        self._pending_assembly = []
        nid = self.free_neurons.pop()
        p = self.p
        for pre in members:
            self._add_synapse(pre, nid, p.w_exc_init, 0, p.delay_exc)
        self.recruited += 1

    def mark_assembly(self, cells: list[int]) -> None:
        self._pending_assembly = list(cells)

    def _add_synapse(self, pre: int, post: int, w0: float, inh: int, delay: int) -> None:
        at = self.offsets[pre + 1]
        self.dst.insert(at, post)
        self.w.insert(at, w0)
        self.inh.insert(at, inh)
        self.dly.insert(at, delay)
        self.elig.insert(at, 0.0)
        offs = self.offsets
        for k in range(pre + 1, len(offs)):
            offs[k] += 1
        self.nsyn += 1

    # ---------------- dopamina y consolidacion ----------------

    def reward(self, amount: float) -> None:
        """Un pulso de dopamina NO cambia pesos. Marca lo que estaba eligible y
        deja que el aprendizaje ocurra en la relajacion."""
        self.da = max(-2.0, min(2.0, self.da + amount))

    def consolidate(self, steps: int = 30, tau: float = 60.0) -> int:
        """Ciclo de reposo: nadie entra, la dopamina decae, la elegibilidad se
        escribe en el peso. Aqui es donde se vuelve memoria. Devuelve cuantas
        sinapsis cambiaron."""
        p = self.p
        w = self.w
        elig = self.elig
        dirty = self._dirty_syn
        changed = 0
        da0 = self.da
        for k in range(steps):
            da = da0 * math.exp(-k / tau)
            if abs(da) < 1e-3:
                break
            for s in tuple(dirty):
                e = elig[s]
                if e == 0.0:
                    continue
                nw = w[s] + p.lr_da * e * da
                if nw < 0.0:
                    nw = 0.0
                elif nw > p.w_max:
                    nw = p.w_max
                if nw != w[s]:
                    w[s] = nw
                    changed += 1
        for s in tuple(dirty):
            elig[s] = 0.0
        dirty.clear()
        self.da = 0.0
        return changed

    def synaptic_scaling(self, target: float | None = None,
                         rate: float = 0.15) -> float:
        """Homeostasis sinaptica: mantiene constante la suma de pesos entrantes
        de cada neurona.

        Sin esto el sistema es inestable por construccion. La depresion STDP se
        aplica cada vez que la postsinaptica dispara, mientras que la
        potenciacion necesita que ademas la presinaptica haya disparado. Es
        decir: la depresion es incondicional y la potenciacion condicional. El
        resultado inevitable es que todos los pesos derivan hacia cero.

        La solucion biologica no es un learning rate mas pequeno: es que la
        neurona defiende su suma de entrada. Si el total baja, todos sus pesos
        suben proporcionalmente; si sube, bajan. Se conservan las diferencias
        RELATIVAS (que es donde vive lo aprendido) y se fija la escala absoluta.

        Devuelve el factor de escala medio aplicado.
        """
        if target is None:
            target = self.p.w_exc_init * max(1.0, self.p.k_target)
        inc, inc_sid = self.inc, self.inc_sid
        w = self.w
        factors = 0.0
        n_used = 0
        for post in range(self.n):
            lo, hi = inc[post], inc[post + 1]
            if hi <= lo:
                continue
            total = 0.0
            for k in range(lo, hi):
                s = inc_sid[k]
                if not self.inh[s]:
                    total += w[s]
            if total <= 1e-9:
                continue
            f = 1.0 + rate * (target / total - 1.0)
            if f < 0.85:
                f = 0.85
            elif f > 1.15:
                f = 1.15
            for k in range(lo, hi):
                s = inc_sid[k]
                if not self.inh[s]:
                    nw = w[s] * f
                    w[s] = 0.0 if nw < 0.0 else (self.p.w_max if nw > self.p.w_max else nw)
            factors += f
            n_used += 1
        return factors / max(1, n_used)

    def prune(self, min_w: float = 0.5, keep_frac: float = 0.02) -> int:
        """Podar por inutilidad: las sinapsis debiles se van; se conserva un
        minimo por neurona para que nadie quede incomunicado."""
        w = self.w
        dst = self.dst
        offs = self.offsets
        keep = bytearray(self.nsyn)
        removed = 0
        for pre in range(self.n):
            lo, hi = offs[pre], offs[pre + 1]
            if hi <= lo:
                continue
            order = sorted(range(lo, hi), key=lambda s: w[s], reverse=True)
            keep_n = max(1, int((hi - lo) * keep_frac))
            for s in order[:keep_n]:
                keep[s] = 1
            for s in range(lo, hi):
                if keep[s]:
                    continue
                if w[s] < min_w:
                    removed += 1
                else:
                    keep[s] = 1
        if removed == 0:
            return 0
        nd, nw, ni, ndly, ne = array("i"), array("d"), bytearray(), array("b"), array("d")
        noff = array("i", [0])
        d = dst
        for pre in range(self.n):
            for s in range(offs[pre], offs[pre + 1]):
                if keep[s]:
                    nd.append(d[s])
                    nw.append(w[s])
                    ni.append(self.inh[s])
                    ndly.append(self.dly[s])
                    ne.append(0.0)
            noff.append(len(nd))
        self.offsets, self.dst, self.w = noff, nd, nw
        self.inh, self.dly, self.elig = ni, ndly, ne
        self.nsyn = len(nd)
        self._dirty_syn.clear()
        self.rebuild_incoming()
        return removed

    # ---------------- telemetria ----------------

    def stats(self) -> dict:
        used = sum(1 for c in self.spike_count if c > 0)
        return {
            "t": self.t,
            "n": self.n,
            "synapses": self.nsyn,
            "total_spikes": self.total_spikes,
            "active": len(self._active),
            "neurons_used": used,
            "recruited": self.recruited,
            "da": round(self.da, 4),
            "w_mean": round(sum(self.w) / max(1, self.nsyn), 4),
        }

    def save(self, path: str) -> None:
        """Formato plano y estable: una linea de cabecera JSON y luego las
        columnas del CSR. Se puede inspeccionar con zcat, no hace falta
        deserializar nada para saber que hay dentro."""
        import gzip as _gz
        head = {
            "version": 1,
            "n": self.n,
            "t": self.t,
            "nsyn": self.nsyn,
            "params": asdict(self.p),
            "spike_count": list(self.spike_count),
            "is_inh": list(self.is_inh),
        }
        with _gz.open(path, "wt") as f:
            f.write(json.dumps(head) + "\n")
            f.write(",".join(map(str, self.offsets)) + "\n")
            f.write(",".join(map(str, self.dst)) + "\n")
            f.write(",".join(f"{x:.4f}" for x in self.w) + "\n")
            f.write("".join("1" if b else "0" for b in self.inh) + "\n")
            f.write(",".join(map(str, self.dly)) + "\n")

    def load(self, path: str) -> None:
        import gzip as _gz
        from dataclasses import fields
        with _gz.open(path, "rt") as f:
            head = json.loads(f.readline())
            n = head["n"]
            self.offsets = array("i", map(int, f.readline().split(",")))
            self.dst = array("i", map(int, f.readline().split(",")))
            self.w = array("d", map(float, f.readline().split(",")))
            self.inh = bytearray(int(c) for c in f.readline().strip())
            self.dly = array("b", map(int, f.readline().split(",")))
            self.n = n
            self.t = head["t"]
            self.nsyn = head["nsyn"]
            self.spike_count = array("i", head["spike_count"])
            self.is_inh = bytearray(head["is_inh"])
            known = {fld.name for fld in fields(Params)}
            self.p = Params(**{k: v for k, v in head["params"].items() if k in known})
            self.elig = array("d", [0.0] * self.nsyn)
            self.v = array("d", [self.p.v_rest] * self.n)
            self.theta = array("d", [0.0] * self.n)
            self.post_trace = array("d", [0.0] * self.n)
            self.last_spike = array("i", [-100000] * self.n)
            self.refract = array("i", [0] * self.n)
            self.budget_spent = array("i", [0] * self.n)
            self.budget_start = array("i", [0] * self.n)
            self.is_sensory = bytearray(self.n)
            self._active.clear()
            self._active_next.clear()
            self._dirty_syn.clear()
            self.free_neurons = [i for i in range(self.n) if self.spike_count[i] == 0]
            self.rebuild_incoming()
