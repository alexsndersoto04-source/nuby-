"""
IA MÍNIMA — una red neuronal real, escrita a mano, sin librerías.

Qué es:
    Un modelo de lenguaje (estilo Bengio 2003). Mira los últimos N caracteres
    y aprende a predecir el siguiente. Sin PyTorch, sin NumPy, sin pesos
    preentrenados: solo matemáticas y listas de Python.

    Entrena con descenso de gradiente + Adam: predice, mide el error,
    retropropaga cuánto debe corregir cada número, corrige. Repetido cientos
    de veces, los números acaban codificando el idioma del corpus.

Cómo correrlo:
    python3 ia_minima.py --pasos 600

Qué se ve:
    La pérdida baja y el texto generado pasa de basura a algo parecido al
    corpus. Eso es una IA aprendiendo de verdad, a escala de juguete.

Por qué es de juguete:
    ~11.000 parámetros y 2 KB de texto. Un modelo que conversa tiene del orden
    de 100.000.000.000 de parámetros y billones de palabras.
    Misma idea. Otra escala de cómputo.
"""

import argparse
import math
import random

# --------------------------------------------------------------------------
# 1. Corpus: el único material del que sale todo lo que la red sabe.
# --------------------------------------------------------------------------
CORPUS = """
el gato duerme en la ventana.
el gato mira la luna.
la luna sale despacio sobre el pueblo.
el pueblo duerme sin ruido.
la ventana brilla con la luz de la luna.
el gato sueña con el río.
el río corre cerca del pueblo.
el río lleva agua clara y fría.
la luna mira el río desde el cielo.
el gato camina por el tejado.
el tejado es viejo y gris.
la noche es larga y tranquila.
el viento mueve la puerta.
la puerta cruje sin prisa.
el gato escucha el viento.
el viento trae olor a lluvia.
la lluvia cae sobre el tejado.
la lluvia moja la ventana.
el gato mira la lluvia.
la lluvia termina al amanecer.
el sol sale sobre el pueblo.
el pueblo despierta despacio.
el gato duerme en la ventana.
la luna se va y el sol llega.
el río sigue corriendo.
el tejado se seca con el sol.
la ventana se llena de luz.
el gato abre los ojos.
el gato mira el sol.
el sol calienta el tejado.
la mañana es clara y tranquila.
"""
CORPUS = " ".join(CORPUS.split()) + " "

CHARS = sorted(set(CORPUS))
V = len(CHARS)
STOI = {c: i for i, c in enumerate(CHARS)}
ITOS = {i: c for c, i in STOI.items()}

CONTEXTO = 8    # cuántos caracteres mira hacia atrás
DIM = 16        # dimensión de la representación de cada carácter
OCULTA = 64     # neuronas de la capa intermedia
ENTRADA = CONTEXTO * DIM


def matriz(filas, cols, escala):
    return [[random.gauss(0, escala) for _ in range(cols)] for _ in range(filas)]


def vector(n, valor=0.0):
    return [valor] * n


def log_softmax_estable(logits):
    mx = max(logits)
    ex = [math.exp(z - mx) for z in logits]
    total = sum(ex)
    return [e / total for e in ex]


class Modelo:
    """Pesos + paso hacia adelante + paso hacia atrás + Adam."""

    def __init__(self, semilla=0):
        random.seed(semilla)
        self.E = matriz(V, DIM, 0.1)
        self.W1 = matriz(ENTRADA, OCULTA, 1 / math.sqrt(ENTRADA))
        self.b1 = vector(OCULTA)
        self.W2 = matriz(OCULTA, V, 1 / math.sqrt(OCULTA))
        self.b2 = vector(V)
        self._adam()

    # --- utilidades de parámetros ---
    def _adam(self):
        self.pesos = [self.E, self.W1, self.b1, self.W2, self.b2]
        self.m, self.v = [], []
        for p in self.pesos:
            if isinstance(p[0], list):                 # matriz
                self.m.append([[0.0] * len(fila) for fila in p])
                self.v.append([[0.0] * len(fila) for fila in p])
            else:                                      # vector
                self.m.append([0.0] * len(p))
                self.v.append([0.0] * len(p))
        self.paso = 0

    def contar_parametros(self):
        return V * DIM + ENTRADA * OCULTA + OCULTA + OCULTA * V + V

    # --- adelante ---
    def adelante(self, lote):
        E, W1, b1, W2, b2 = self.E, self.W1, self.b1, self.W2, self.b2
        xs, hs, probs = [], [], []
        for contexto, _objetivo in lote:
            x = []
            for c in contexto:
                x.extend(E[c])
            xs.append(x)

            h = [0.0] * OCULTA
            for j in range(OCULTA):
                s = b1[j]
                w = W1
                for k in range(ENTRADA):
                    s += x[k] * w[k][j]
                h[j] = math.tanh(s)
            hs.append(h)

            logits = [0.0] * V
            for j in range(OCULTA):
                hj = h[j]
                wj = W2[j]
                for v in range(V):
                    logits[v] += hj * wj[v]
            for v in range(V):
                logits[v] += b2[v]
            probs.append(log_softmax_estable(logits))
        return xs, hs, probs

    # --- atrás ---
    def atras(self, lote, xs, hs, probs):
        B = len(lote)
        gE = [[0.0] * DIM for _ in range(V)]
        gW1 = [[0.0] * OCULTA for _ in range(ENTRADA)]
        gb1 = [0.0] * OCULTA
        gW2 = [[0.0] * V for _ in range(OCULTA)]
        gb2 = [0.0] * V
        perdida = 0.0

        for i, (_contexto, objetivo) in enumerate(lote):
            p = probs[i]
            perdida -= math.log(max(p[objetivo], 1e-12))

            dlogits = [x / B for x in p]
            dlogits[objetivo] -= 1.0 / B

            h = hs[i]
            dh = [0.0] * OCULTA
            for j in range(OCULTA):
                hj = h[j]
                wj = self.W2[j]
                gw = gW2[j]
                acc = 0.0
                for v in range(V):
                    dv = dlogits[v]
                    gw[v] += hj * dv
                    acc += wj[v] * dv
                dh[j] = acc * (1.0 - hj * hj)
            for v in range(V):
                gb2[v] += dlogits[v]

            x = xs[i]
            for k in range(ENTRADA):
                xk = x[k]
                row = gW1[k]
                for j in range(OCULTA):
                    row[j] += xk * dh[j]
            for j in range(OCULTA):
                gb1[j] += dh[j]

            # gradiente de las representaciones de cada carácter del contexto
            dx = [0.0] * ENTRADA
            for k in range(ENTRADA):
                s = 0.0
                wk = self.W1[k]
                for j in range(OCULTA):
                    s += wk[j] * dh[j]
                dx[k] = s
            for pos, c in enumerate(lote[i][0]):
                fila = gE[c]
                base = pos * DIM
                for d in range(DIM):
                    fila[d] += dx[base + d]

        return perdida / B, [gE, gW1, gb1, gW2, gb2]

    # --- Adam ---
    def actualizar(self, grads, lr=0.02, b1=0.9, b2=0.999, eps=1e-8):
        self.paso += 1
        t = self.paso
        c1 = 1 - b1 ** t
        c2 = 1 - b2 ** t
        for idx, (p, g, m, v) in enumerate(zip(self.pesos, grads, self.m, self.v)):
            if isinstance(p[0], list):
                for r in range(len(p)):
                    pr, gr, mr, vr = p[r], g[r], m[r], v[r]
                    for c in range(len(pr)):
                        gv = gr[c]
                        mr[c] = b1 * mr[c] + (1 - b1) * gv
                        vr[c] = b2 * vr[c] + (1 - b2) * gv * gv
                        pr[c] -= lr * (mr[c] / c1) / (math.sqrt(vr[c] / c2) + eps)
            else:
                for c in range(len(p)):
                    gv = g[c]
                    m[c] = b1 * m[c] + (1 - b1) * gv
                    v[c] = b2 * v[c] + (1 - b2) * gv * gv
                    p[c] -= lr * (m[c] / c1) / (math.sqrt(v[c] / c2) + eps)


def ejemplos(corpus, codigos, contexto):
    datos = []
    for i in range(len(codigos) - contexto):
        datos.append((codigos[i:i + contexto], codigos[i + contexto]))
    return datos


def perdida_total(modelo, datos):
    """Perplejidad aproximada sobre todo el corpus (sin tocar los pesos)."""
    total, n = 0.0, 0
    lote = 64
    for i in range(0, len(datos), lote):
        trozo = datos[i:i + lote]
        _, _, probs = modelo.adelante(trozo)
        for j, (_c, y) in enumerate(trozo):
            total -= math.log(max(probs[j][y], 1e-12))
            n += 1
    return total / n


def generar(modelo, inicio, cuantos=140, temperatura=0.8, semilla=1):
    rng = random.Random(semilla)
    contexto = [STOI[c] for c in inicio]
    salida = list(inicio)
    for _ in range(cuantos):
        _, _, probs = modelo.adelante([(contexto[-CONTEXTO:], 0)])
        p = probs[0]
        if temperatura != 1.0:
            p = [x ** (1.0 / temperatura) for x in p]
            s = sum(p)
            p = [x / s for x in p]
        r, acc = rng.random(), 0.0
        elegido = len(p) - 1
        for i, prob in enumerate(p):
            acc += prob
            if r <= acc:
                elegido = i
                break
        salida.append(ITOS[elegido])
        contexto.append(elegido)
    return "".join(salida)


def comprobar_gradiente(modelo, lote, eps=1e-4, cuantos=6):
    """Compara el gradiente analítico con el numérico. Si coinciden, la
    retropropagación está bien escrita: no hay trampa."""
    xs, hs, probs = modelo.adelante(lote)
    _, grads = modelo.atras(lote, xs, hs, probs)
    comprobaciones = []

    def perdida_con():
        xs2, hs2, pr2 = modelo.adelante(lote)
        return sum(-math.log(max(pr2[i][y], 1e-12)) for i, (_c, y) in enumerate(lote)) / len(lote)

    # E[0][0]
    orig = modelo.E[0][0]
    modelo.E[0][0] = orig + eps
    lp = perdida_con()
    modelo.E[0][0] = orig - eps
    lm = perdida_con()
    modelo.E[0][0] = orig
    comprobaciones.append(("E[0][0]", (lp - lm) / (2 * eps), grads[0][0][0]))

    # W1[3][5]
    orig = modelo.W1[3][5]
    modelo.W1[3][5] = orig + eps
    lp = perdida_con()
    modelo.W1[3][5] = orig - eps
    lm = perdida_con()
    modelo.W1[3][5] = orig
    comprobaciones.append(("W1[3][5]", (lp - lm) / (2 * eps), grads[1][3][5]))

    # W2[7][2]
    orig = modelo.W2[7][2]
    modelo.W2[7][2] = orig + eps
    lp = perdida_con()
    modelo.W2[7][2] = orig - eps
    lm = perdida_con()
    modelo.W2[7][2] = orig
    modelo.W2[7][2] = orig
    comprobaciones.append(("W2[7][2]", (lp - lm) / (2 * eps), grads[3][7][2]))

    # b2[1]
    orig = modelo.b2[1]
    modelo.b2[1] = orig + eps
    lp = perdida_con()
    modelo.b2[1] = orig - eps
    lm = perdida_con()
    modelo.b2[1] = orig
    comprobaciones.append(("b2[1]", (lp - lm) / (2 * eps), grads[4][1]))

    print("\nVerificación del gradiente (analítico vs numérico):")
    for nombre, num, ana in comprobaciones[:cuantos]:
        ok = abs(num - ana) < 1e-4 * max(1.0, abs(num))
        print(f"  {nombre:<10} numérico={num:+.6f}  analítico={ana:+.6f}  {'OK' if ok else 'DIFERENTE'}")


def main():
    ap = argparse.ArgumentParser(description="IA mínima desde cero, sin librerías")
    ap.add_argument("--pasos", type=int, default=2500)
    ap.add_argument("--lote", type=int, default=16)
    ap.add_argument("--lr", type=float, default=0.004)
    ap.add_argument("--semilla", type=int, default=0)
    ap.add_argument("--verificar-gradiente", action="store_true")
    ap.add_argument("--cada", type=int, default=500, help="cada cuántos pasos mostrar muestra")
    args = ap.parse_args()

    codigos = [STOI[c] for c in CORPUS]
    datos = ejemplos(CORPUS, codigos, CONTEXTO)
    modelo = Modelo(args.semilla)
    rng = random.Random(args.semilla + 7)

    print(f"Corpus: {len(CORPUS)} caracteres | vocabulario: {V} | "
          f"parámetros: {modelo.contar_parametros():,}")
    print(f"Ejemplos de entrenamiento: {len(datos):,}\n")

    if args.verificar_gradiente:
        comprobar_gradiente(modelo, datos[:8])

    inicio_perdida = perdida_total(modelo, datos)
    print(f"Pérdida inicial (sin entrenar): {inicio_perdida:.4f}")
    print("Muestra inicial:", repr(generar(modelo, "el gato ", 70)))

    historial = []
    for paso in range(1, args.pasos + 1):
        lote = [datos[rng.randrange(len(datos))] for _ in range(args.lote)]
        xs, hs, probs = modelo.adelante(lote)
        perdida, grads = modelo.atras(lote, xs, hs, probs)
        modelo.actualizar(grads, lr=args.lr)

        if paso % 50 == 0:
            historial.append(perdida)
        if paso % args.cada == 0 or paso == args.pasos:
            print(f"paso {paso:>4} | pérdida {perdida:.4f} | "
                  f"perplejidad {math.exp(perdida):6.2f}")

    final = perdida_total(modelo, datos)
    print(f"\nPérdida final (todo el corpus): {final:.4f}")
    print("\nCurva de aprendizaje (cada barra = 50 pasos, más alta = más error):")
    alto = 12
    techo = max(historial) if historial else 1.0
    for nivel in range(alto, 0, -1):
        linea = "".join("█" if (h / techo) * alto >= nivel else " " for h in historial)
        print(f"  {linea}")
    print(f"  {'-' * len(historial)}")
    print(f"  empezó en {math.exp(inicio_perdida):.1f} opciones de duda, "
          f"acabó en {math.exp(final):.2f}")
    for t in (0.5, 0.8):
        print(f"\nMuestra entrenada (temperatura {t}):")
        print(" ", generar(modelo, "el gato ", 160, temperatura=t))
    print("\nLa perplejidad es 'entre cuántos caracteres duda el modelo'. "
          "Empezó en ~27 y ahora duda de muy poco: eso es aprender.")


if __name__ == "__main__":
    main()
