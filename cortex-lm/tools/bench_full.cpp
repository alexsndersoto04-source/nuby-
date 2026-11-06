// bench_full — banco para texto GRANDE (millones de simbolos).
//
// POR QUE EXISTE
// --------------
// Los bancos anteriores guardan en memoria todas las features (N vectores de
// 1024 floats). Con 78.000 simbolos son 320 MB; con 4,5 millones, 18 GB: no
// cabe. Aqui se entrena EN FLUJO: se lee el texto una vez, se calculan las
// features al vuelo y se tira la matriz P del pasado. Memoria constante.
//
// QUE MIDE
// --------
// Dos lectores, ambos LOCALES (sin retropropagacion):
//   A) sobre el estado de la red de pulsos (lo que hay hoy)
//   B) sobre contexto directo (los ultimos C simbolos en one-hot)
//
// El segundo esta para responder una pregunta incomoda: si el contexto directo
// gana a la red de pulsos, entonces la red no esta aportando representacion,
// solo decoracion. Y eso hay que saberlo antes de seguir.
//
// Uso:
//   bench_full NTR NTE K LH C TLR [SEED]
//     NTR  simbolos de entrenamiento
//     NTE  simbolos de test (salen del final del texto, nunca vistos)
//     K    neuronas por estado (feature de la red)
//     LH   estados en la ventana
//     C    caracteres de contexto directo (0 = no usar)
//     TLR  tasa de aprendizaje del lector, en nats (0.02 = suave)
//
// Compilar:
//   g++ -O2 -std=c++17 -Iengine engine/cortex_lm.cpp tools/bench_full.cpp \
//       -o bin/bench_full

#include "cortex_lm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace cortex;

static std::vector<unsigned char> load(const char *path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "no se puede abrir %s\n", path);
        std::exit(1);
    }
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------- lector
// Un lector de una capa con softmax sobre UNICAMENTE las clases que existen en
// el corpus. Entrenamiento en linea con el error local de cada salida.
// Vectorizado sobre las features activas: z es disperso (C caracteres + K
// neuronas), asi que tocar solo los indices activos lo hace O(activos) en vez
// de O(F).
struct Lector {
    int F = 0, C = 0;
    std::vector<double> W;       // C x F
    std::vector<double> b;       // C
    std::vector<int> voc;        // indice de clase -> simbolo real
    std::vector<int> pos;        // simbolo real -> indice de clase (-1 si no)
    std::vector<double> lg;
    double tasa = 0.02;
    double decae = 0.0;   // 0 = tasa fija; si no, tasa / (1 + t/decae)
    long   paso_n = 0;

    double tasa_ahora() const {
        return decae > 0.0 ? tasa / (1.0 + (double)paso_n / decae) : tasa;
    }

    void init(int nf, const std::vector<int> &clases, double lr) {
        F = nf;
        C = (int)clases.size();
        voc = clases;
        pos.assign(256, -1);
        for (int i = 0; i < C; ++i) pos[voc[i]] = i;
        W.assign((size_t)C * F, 0.0);
        b.assign(C, 0.0);
        lg.assign(C, 0.0);
        tasa = lr;
    }

    // z: vector disperso (indices activos y sus valores)
    double paso(const std::vector<int> &idx, const std::vector<double> &val,
                int y_real, double T) {
        ++paso_n;
        const int m = (int)idx.size();
        // logits solo sobre las features activas
        double mx = -1e30;
        for (int c = 0; c < C; ++c) {
            const double *w = &W[(size_t)c * F];
            double v = b[c];
            for (int k = 0; k < m; ++k) v += w[idx[k]] * val[k];
            lg[c] = v;
            if (v > mx) mx = v;
        }
        double sm = 0.0;
        for (int c = 0; c < C; ++c) {
            lg[c] = std::exp((lg[c] - mx) / T);
            sm += lg[c];
        }
        const int y = pos[y_real];
        if (y < 0) return std::log2(256.0);          // clase imposible
        const double p = std::max(1e-12, lg[y] / sm);
        const double bits = -std::log2(p);
        // descenso del error (regla local: cada salida con SU error)
        const double escala = tasa_ahora() / (T * sm);
        for (int c = 0; c < C; ++c) {
            const double g = (lg[c] / sm - (c == y ? 1.0 : 0.0)) * escala;
            if (g == 0.0) continue;
            double *w = &W[(size_t)c * F];
            for (int k = 0; k < m; ++k) w[idx[k]] -= g * val[k];
            b[c] -= g;
        }
        return bits;
    }

    double evaluar(const std::vector<int> &idx, const std::vector<double> &val,
                   int y_real, double T) {
        const int m = (int)idx.size();
        double mx = -1e30;
        for (int c = 0; c < C; ++c) {
            const double *w = &W[(size_t)c * F];
            double v = b[c];
            for (int k = 0; k < m; ++k) v += w[idx[k]] * val[k];
            lg[c] = v;
            if (v > mx) mx = v;
        }
        double sm = 0.0;
        for (int c = 0; c < C; ++c) {
            lg[c] = std::exp((lg[c] - mx) / T);
            sm += lg[c];
        }
        const int y = pos[y_real];
        if (y < 0) return std::log2(256.0);
        return -std::log2(std::max(1e-12, lg[y] / sm));
    }
};

// ---------------------------------------------------------------- referencia
// Modelo de contexto de C caracteres: cuenta frecuencias. Es el techo
// alcanzable con ESE contexto y ESE texto. Sin el, cualquier bpc es un numero
// sin significado.
struct Ref {
    // Contador de frecuencias de orden C con RETROCESO a orden menor.
    // Sin retroceso, un contexto nunca visto se castiga con 16 bits y el modelo
    // de orden alto parece malisimo solo por falta de datos: la referencia
    // quedaria debil y ganarle no significaria nada. Con retroceso, la
    // referencia es lo mejor que se puede hacer con ESE contexto, que es lo que
    // hay que batir.
    std::vector<std::map<std::string, std::map<int, int>>> ord;  // por orden
    std::vector<std::map<std::string, int>> tot;                 // por orden
    std::map<int, int> uni;
    int C = 0;
    int A = 256;
    double alfa = 1.0;   // peso del retroceso (Jelinek-Mercer)

    void entrena(const std::vector<unsigned char> &t, int n) {
        ord.assign((size_t)C + 1, {});
        tot.assign((size_t)C + 1, {});
        for (int i = 0; i + 1 < n; ++i) uni[t[i]]++;
        for (int o = 1; o <= C; ++o) {
            for (int i = 0; i + o < n; ++i) {
                const std::string k((const char *)&t[i], (size_t)o);
                ord[o][k][t[i + o]]++;
                tot[o][k]++;
            }
        }
    }

    // p(y | ultimos `o` simbolos que terminan en `fin`) con retroceso a o-1.
    // `fin` apunta al ULTIMO simbolo del contexto: al bajar de orden se quitan
    // los mas lejanos, que es lo unico que tiene sentido (quitar los cercanos
    // convierte la referencia en ruido).
    double p(int y, const unsigned char *fin, int o) const {
        if (o == 0) {
            double s = 0.0;
            for (auto &kv : uni) s += kv.second;
            const auto it = uni.find(y);
            const double cnt = it == uni.end() ? 0.0 : (double)it->second;
            return (cnt + 0.1) / (s + 0.1 * A);
        }
        const std::string k((const char *)(fin - (o - 1)), (size_t)o);
        const auto it = ord[o].find(k);
        const double baja = p(y, fin, o - 1);
        if (it == ord[o].end()) return baja;
        const double t_ = (double)tot[o].at(k);
        const auto jt = it->second.find(y);
        const double cnt = jt == it->second.end() ? 0.0 : (double)jt->second;
        return (cnt + alfa * baja) / (t_ + alfa);
    }

    double evalua(const std::vector<unsigned char> &t, int desde) const {
        double bits = 0.0;
        int m = 0;
        for (size_t i = (size_t)desde; i + (size_t)C < t.size(); ++i) {
            const double q = p(t[i + C], &t[i + C - 1], C);
            bits += -std::log2(std::max(1e-12, q));
            ++m;
        }
        return bits / std::max(1, m);
    }
};

int main(int argc, char **argv) {
    const int NTR = argc > 1 ? std::atoi(argv[1]) : 60000;
    const int NTE = argc > 2 ? std::atoi(argv[2]) : 10000;
    const int K   = argc > 3 ? std::atoi(argv[3]) : 64;
    const int LH  = argc > 4 ? std::atoi(argv[4]) : 8;
    const int CC  = argc > 5 ? std::atoi(argv[5]) : 0;   // contexto directo
    const double TLR = argc > 6 ? std::atof(argv[6]) : 0.02;
    const unsigned SEED = argc > 7 ? (unsigned)std::atoi(argv[7]) : 1234u;
    // Pasos tras los cuales la tasa se divide por 2 (0 = tasa fija). Con
    // millones de simbolos hace falta: una tasa fija deja de converger.
    const double DEC = argc > 8 ? std::atof(argv[8]) : 0.0;
    // Pasadas sobre el train con el lector (2 por defecto). Sirve para separar
    // "el lector no ha aprendido" de "la representacion no tiene la informacion".
    const int EPOCAS = argc > 9 ? std::atoi(argv[9]) : 2;

    auto train = load("data/spanish_small.txt");
    auto test  = load("data/spanish_holdout.txt");
    if ((int)train.size() < NTR) {
        std::fprintf(stderr, "el train tiene %zu, se pidieron %d\n",
                     train.size(), NTR);
        return 1;
    }

    // --- red ---
    Params p;
    p.seed = SEED;
    Engine e(p);
    e.build_random(p.seed);
    const int Dd = p.layer_size;

    // --- vocabulario: solo los simbolos que existen en el train ---
    std::vector<int> visto(256, 0);
    for (int i = 0; i < NTR; ++i) visto[train[i]] = 1;
    std::vector<int> clases;
    for (int c = 0; c < 256; ++c) if (visto[c]) clases.push_back(c);
    const int C = (int)clases.size();

    // --- dimensiones de la feature ---
    const int FR = K > 0 ? K * LH : 0;  // estado de la red (K=0: sin red)
    const int FC = CC > 0 ? CC * 256 : 0;   // contexto directo
    const int F  = FR + FC;
    Lector lec;
    lec.init(F, clases, TLR);
    lec.decae = DEC;

    std::printf("train %d | test %d | alfabeto %d | features %d "
                "(red %d + contexto %d)\n", NTR, NTE, C, F, FR, FC);

    // --- buffers reutilizados (memoria constante) ---
    std::vector<std::vector<float>> ring(LH, std::vector<float>(Dd, 0.0f));
    std::vector<float> f;
    std::vector<int> idx;
    std::vector<double> val;
    idx.reserve(F);
    val.reserve(F);

    // Seleccion de las K neuronas mas informativas: se elige con una pasada
    // corta sobre el train (las de mayor varianza de fatiga).
    std::vector<int> sel(std::max(1, K));
    if (K > 0) {
        const int Ncal = std::min(NTR, 8000);
        std::vector<std::vector<float>> S(Ncal);
        e.reset_state();
        for (int t = 0; t < Ncal; ++t) {
            e.feed(train[t]);
            e.theta_features(f);
            S[t] = f;
        }
        std::vector<double> mean(Dd, 0.0);
        for (int j = 0; j < Dd; ++j) {
            double a = 0;
            for (int t = 0; t < Ncal; ++t) a += S[t][j];
            mean[j] = a / Ncal;
        }
        std::vector<std::pair<double, int>> var(Dd);
        for (int j = 0; j < Dd; ++j) {
            double v = 0;
            for (int t = 0; t < Ncal; ++t) {
                const double d = S[t][j] - mean[j];
                v += d * d;
            }
            var[j] = {v, j};
        }
        std::sort(var.rbegin(), var.rend());
        for (int k = 0; k < K; ++k) sel[k] = var[k].second;
    }
    if (K == 0 && CC == 0) {
        std::fprintf(stderr, "K=0 y C=0: no hay nada que medir\n");
        return 1;
    }

    // Construye las features dispersas de un instante.
    auto features = [&](const std::vector<unsigned char> &txt, size_t t) {
        idx.clear();
        val.clear();
        // (1) estado de la red: ventana de LH estados, K neuronas cada uno
        for (int h = 0; h < LH && K > 0; ++h) {
            for (int k = 0; k < K; ++k) {
                const float v = ring[h][sel[k]];
                if (v == 0.0f) continue;
                idx.push_back(h * K + k);
                val.push_back(v);
            }
        }
        // (2) contexto directo: ultimos CC simbolos en one-hot
        if (CC > 0) {
            for (int c = 0; c < CC; ++c) {
                const long k = (long)t - c;
                if (k < 0) continue;
                const int sim = txt[(size_t)k];
                idx.push_back(FR + c * 256 + sim);
                val.push_back(1.0);
            }
        }
    };

    // --- entrenamiento en flujo ---
    std::printf("entrenando...\n");
    std::fflush(stdout);
    e.reset_state();
    for (int h = 0; h < LH; ++h) std::fill(ring[h].begin(), ring[h].end(), 0.0f);

    auto etapa = [&](const std::vector<unsigned char> &txt, int n, bool aprender,
                     double T) -> std::pair<double, int> {
        double bits = 0.0;
        int m = 0;
        e.reset_state();
        for (int h = 0; h < LH; ++h) std::fill(ring[h].begin(), ring[h].end(), 0.0f);
        for (int t = 0; t + 1 < n; ++t) {
            e.feed(txt[t]);
            e.theta_features(f);
            for (int h = LH - 1; h > 0; --h) ring[h] = ring[h - 1];
            ring[0] = f;
            features(txt, (size_t)t);
            const int y = txt[t + 1];
            if (aprender) bits += lec.paso(idx, val, y, T);
            else          bits += lec.evaluar(idx, val, y, T);
            ++m;
        }
        return {bits / std::max(1, m), m};
    };

    // dos pasadas: la segunda consolida (equivale a 2 epochs, barato)
    for (int ep = 0; ep < EPOCAS; ++ep) {
        auto r = etapa(train, NTR, true, 1.0);
        std::printf("  epoca %d: %.3f bpc (train)\n", ep + 1, r.first);
        std::fflush(stdout);
    }

    // --- test: los ultimos NTE del holdout, nunca vistos ---
    std::vector<unsigned char> te(test.begin(),
                                  test.begin() + std::min((int)test.size(), NTE));
    // temperatura elegida en el PRIMER tercio del test, medida en el resto
    const int cal = (int)te.size() / 3;
    std::vector<unsigned char> tcal(te.begin(), te.begin() + cal);
    std::vector<unsigned char> tev(te.begin() + cal, te.end());
    double mejorT = 1.0, mejorB = 1e30;
    for (double T : {1.0, 0.7, 0.5, 0.3, 0.2, 0.15, 0.1, 0.07}) {
        auto r = etapa(tcal, (int)tcal.size(), false, T);
        if (r.first < mejorB) { mejorB = r.first; mejorT = T; }
    }
    auto res = etapa(tev, (int)tev.size(), false, mejorT);

    // --- referencias del mismo contexto ---
    // La unica comparacion que vale: la red/lector con C de contexto contra un
    // contador de frecuencias con ESE MISMO C. Si no gana ahi, no aporta.
    auto ref_de = [&](int c) {
        Ref r; r.C = c; r.A = C; r.entrena(train, NTR);
        return r.evalua(tev, 0);
    };
    const double b1 = ref_de(1);
    const double b2 = ref_de(2);
    const double b3 = ref_de(3);
    const double bm = CC > 0 ? ref_de(CC) : -1.0;

    std::printf("\n--- RESULTADO ---\n");
    std::printf("  sistema (red + lector local)   : %.3f bpc\n", res.first);
    std::printf("  features: red %d (K=%d LH=%d) + contexto %d\n", FR, K, LH, CC);
    std::printf("  referencia 1 caracter         : %.3f bpc\n", b1);
    std::printf("  referencia 2 caracteres       : %.3f bpc\n", b2);
    std::printf("  referencia 3 caracteres       : %.3f bpc\n", b3);
    if (bm > 0.0) {
        std::printf("  referencia %d caracteres        : %.3f bpc  <-- misma "
                    "informacion de contexto\n", CC, bm);
        std::printf("  DIFERENCIA frente a su referencia: %+.3f bpc (%s)\n",
                    res.first - bm, res.first < bm ? "GANA" : "PIERDE");
    }
    std::printf("  temperatura %.2f | simbolos de test %d\n", mejorT, res.second);
    return 0;
}
