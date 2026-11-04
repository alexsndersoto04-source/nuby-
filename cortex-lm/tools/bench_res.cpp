// bench_res — reservorio recurrente ALEATORIO con lector local (RLS/FORCE).
//
// POR QUE EXISTE
// --------------
// Los bancos anteriores usan una VENTANA CONGELADA de los ultimos LH estados.
// Medido (OPCION1-ACTA.md): esa ventana no aporta nada por encima de los 2-3
// ultimos caracteres, y la red empataba con el bigrama. La ventana no olvida
// poco: es que no hay donde guardar el pasado.
//
// Aqui la red es un RESERVORIO: N neuronas recurrentes aleatorias, con estados
// que se actualizan en cada simbolo y NO se reinician. La memoria no es una foto
// de los ultimos simbolos: es un estado que evoluciona. Es lo que se llama
// "estado recurrente comprimido" y es lo que se propuso como Opcion 2.
//
// Se respeta la regla de pureza: el reservorio NO se entrena. Sus pesos son
// aleatorios y se quedan quietos (eso es lo biologico: la conectividad no viene
// de un gradiente). Lo unico que aprende es el lector, con una regla LOCAL
// (cada salida se corrige con SU error, sin retropropagacion).
//
// QUE MIDE
// --------
//  A) reservorio + lector RLS (regla local, FORCE)
//  B) solo los ultimos C caracteres + lector local  (la vara, sin red)
//  C) referencias con retroceso a orden menor: 1, 2, 3 y la MISMA C
//
// Uso:
//   bench_res NTR NTE N ALFA RHO [SEED] [LAMBDA] [P0] [C]
//     NTR    simbolos de entrenamiento (desde el principio del fichero)
//     NTE    simbolos de test (del fichero de test, nunca vistos)
//     N      neuronas del reservorio (0 = sin reservorio)
//     ALFA   ganancia de la entrada
//     RHO    radio espectral aproximado de la matriz recurrente
//     SEED   semilla del reservorio
//     LAMBDA factor de olvido de RLS (1 = sin olvido)
//     P0     valor inicial de la inversa de autocorrelacion
//     C      caracteres de contexto directo anadidos (0 = ninguno)
//
// Compilar:
//   g++ -O2 -std=c++17 -Iengine engine/cortex_lm.cpp tools/bench_res.cpp -o bin/bench_res

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

static std::vector<unsigned char> load(const char *path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "no se puede abrir %s\n", path);
        std::exit(1);
    }
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

// ------------------------------------------------------------------ reservorio
// Estado: x[t] = tanh( g * Win u[t] + rho * W x[t-1] )
// - Win y W son ALEATORIOS y no se tocan nunca.
// - W es DISPERSA (k conexiones por neurona): barata y mas parecida a un cerebro
//   que una matriz densa.
// - Los estados se leen tal cual (sin reiniciar entre simbolos). Esa es toda la
//   memoria: no hay ventana.
struct Reservorio {
    int N = 0, A = 1;
    std::vector<float> Win;      // A x N  (simbolo actual)
    std::vector<float> Win2;     // A x N  (simbolo anterior, ganancia beta)
    // W dispersa: por neurona solo los vecinos con conexion (grado fijo).
    std::vector<int>   widx;     // N x grado
    std::vector<float> wval;     // N x grado
    std::vector<float> x, xn;
    // Corriente de fondo. Medido: 0,05 constante satura el tanh (todo a +1) y
    // empeora 0,58 bpc. Se queda en 0 salvo que se pida lo contrario.
    float sesgo = 0.0f;
    double g = 1.0, rho = 0.9;
    int grado = 8;

    void init(int n, int alfabeto, unsigned seed, double ganancia, double radio,
              int grado_conex = 8) {
        N = n; A = alfabeto; g = ganancia; rho = radio; grado = grado_conex;
        std::mt19937 rng(seed);
        std::normal_distribution<double> nd(0.0, 1.0);
        std::uniform_real_distribution<double> ud(-1.0, 1.0);
        Win.assign((size_t)A * N, 0.0f);
        Win2.assign((size_t)A * N, 0.0f);
        const double s_in = 1.0 / std::sqrt((double)std::max(1, A));
        for (size_t i = 0; i < Win.size(); ++i)
            Win[i] = (float)(nd(rng) * s_in);
        for (size_t i = 0; i < Win2.size(); ++i)
            Win2[i] = (float)(nd(rng) * s_in);

        // Dispersa: `grado` conexiones por neurona, valores +-1 normalizados.
        // Se guarda como listas de vecinos: recorrerla cuesta N*grado y NO N^2.
        grado = std::min(grado, N);
        widx.assign((size_t)N * grado, 0);
        wval.assign((size_t)N * grado, 0.0f);
        const double s_w = 1.0 / std::sqrt((double)grado);
        std::uniform_int_distribution<int> jd(0, N - 1);
        for (int i = 0; i < N; ++i)
            for (int k = 0; k < grado; ++k) {
                widx[(size_t)i * grado + k] = jd(rng);
                wval[(size_t)i * grado + k] = (float)((ud(rng) > 0 ? 1.0 : -1.0) * s_w);
            }
        x.assign(N, 0.0f);
        xn.assign(N, 0.0f);
    }

    void reset() { std::fill(x.begin(), x.end(), 0.0f); previo = -1; }

    int previo = -1;
    double beta = 0.0;   // ganancia del simbolo anterior en la entrada

    void paso(int simbolo) {
        const float *win = &Win[(size_t)simbolo * N];
        const float *win2 = (previo >= 0 && beta != 0.0)
                                ? &Win2[(size_t)previo * N] : nullptr;
        const float bb = (float)beta;
        const float r = (float)rho, gg = (float)g;
        for (int i = 0; i < N; ++i) {
            const int *ix = &widx[(size_t)i * grado];
            const float *wv = &wval[(size_t)i * grado];
            float acc = 0.0f;
            for (int k = 0; k < grado; ++k) acc += wv[k] * x[ix[k]];
            x[i] = std::tanh(r * acc + gg * win[i] + sesgo
                             + (win2 ? bb * win2[i] : 0.0f));
        }
        previo = simbolo;
    }
};

// ---------------------------------------------------------------------- lector
// Una capa, softmax sobre las clases del corpus, regla LOCAL (FORCE/RLS).
//   g  = P z / (lambda + z.P z)
//   P <- (P - g (P z)^T) / lambda
//   w_c <- w_c + g e_c        (cada salida con SU error)
// Entrenamiento EN FLUJO: nunca se guardan todos los estados, memoria constante.
struct Lector {
    int F = 0, C = 0;
    std::vector<double> W, b, lg, P, Pz, g_, z;
    std::vector<int> voc, pos;
    double lambda = 1.0, p0 = 100.0;
    bool rls = true;
    double tasa_grad = 0.02;
    double decae = 0.0;      // 0 = tasa fija; si no, tasa/(1+t/decae)
    long   paso_n = 0;
    bool   adagrad = false;
    std::vector<double> ac;   // acumulador de gradientes al cuadrado

    double tasa_ahora() const {
        return decae > 0.0 ? tasa_grad / (1.0 + (double)paso_n / decae) : tasa_grad;
    }

    void init(int nf, const std::vector<int> &clases, double lam, double p0v, bool usar_rls) {
        F = nf; C = (int)clases.size(); voc = clases;
        pos.assign(256, -1);
        for (int i = 0; i < C; ++i) pos[voc[i]] = i;
        W.assign((size_t)C * F, 0.0);
        b.assign(C, 0.0);
        lg.assign(C, 0.0);
        lambda = lam; p0 = p0v; rls = usar_rls;
        if (rls) {
            P.assign((size_t)(F + 1) * (F + 1), 0.0);
            for (int i = 0; i <= F; ++i) P[(size_t)i * (F + 1) + i] = p0;
            Pz.assign(F + 1, 0.0);
            g_.assign(F + 1, 0.0);
        }
        z.assign(F + 1, 0.0);
    }

    // z se rellena fuera; aqui solo se aprende. Devuelve los bits del simbolo.
    double paso(int y_real) {
        ++paso_n;
        z[F] = 1.0;
        double bits = 0.0;
        const int y = pos[y_real];
        if (y >= 0) {
            double mx = -1e30;
            for (int c = 0; c < C; ++c) {
                double v = b[c];
                const double *w = &W[(size_t)c * F];
                for (int k = 0; k < F; ++k) v += w[k] * z[k];
                lg[c] = v;
                if (v > mx) mx = v;
            }
            double sm = 0.0;
            for (int c = 0; c < C; ++c) { lg[c] = std::exp(lg[c] - mx); sm += lg[c]; }
            bits = -std::log2(std::max(1e-12, lg[y] / sm));

            if (rls) {
                // Pz
                for (int i = 0; i <= F; ++i) {
                    const double *fila = &P[(size_t)i * (F + 1)];
                    double acc = 0.0;
                    for (int k = 0; k <= F; ++k) acc += fila[k] * z[k];
                    Pz[i] = acc;
                }
                double den = lambda;
                for (int i = 0; i <= F; ++i) den += z[i] * Pz[i];
                if (den > 1e-12) {
                    for (int i = 0; i <= F; ++i) g_[i] = Pz[i] / den;
                    for (int i = 0; i <= F; ++i) {
                        double *fila = &P[(size_t)i * (F + 1)];
                        const double gi = g_[i];
                        for (int k = 0; k <= F; ++k)
                            fila[k] = (fila[k] - gi * Pz[k]) / lambda;
                    }
                    for (int c = 0; c < C; ++c) {
                        const double e = (c == y ? 1.0 : 0.0)
                                       - (lg[c] / sm);      // error local
                        if (std::fabs(e) < 1e-12) continue;
                        double *w = &W[(size_t)c * F];
                        for (int k = 0; k < F; ++k) w[k] += g_[k] * e;
                        b[c] += g_[F] * e;
                    }
                }
            } else {
                // descenso del error: cada salida se mueve con SU error.
                // Es local (no hay nada que viaje hacia atras) y cuesta
                // O(clases x rasgos) en vez de O(rasgos^2) como RLS.
                const double escala = tasa_ahora();
                for (int c = 0; c < C; ++c) {
                    const double gr = (lg[c] / sm) - (c == y ? 1.0 : 0.0);
                    if (gr == 0.0) continue;
                    double *w = &W[(size_t)c * F];
                    if (adagrad) {
                        double *a = &ac[(size_t)c * F];
                        for (int k = 0; k < F; ++k) {
                            const double g2 = gr * z[k];
                            a[k] += g2 * g2;
                            w[k] -= escala * g2 / std::sqrt(a[k] + 1e-8);
                        }
                    } else {
                        for (int k = 0; k < F; ++k) w[k] -= escala * gr * z[k];
                    }
                    b[c] -= escala * gr;
                }
            }
        }
        return bits;
    }

    double evaluar(int y_real, double T) const {
        double mx = -1e30, sm = 0.0;
        std::vector<double> v(C);
        for (int c = 0; c < C; ++c) {
            double a = b[c];
            const double *w = &W[(size_t)c * F];
            for (int k = 0; k < F; ++k) a += w[k] * z[k];
            v[c] = a / T;
            if (v[c] > mx) mx = v[c];
        }
        for (int c = 0; c < C; ++c) { v[c] = std::exp(v[c] - mx); sm += v[c]; }
        const int y = pos[y_real];
        if (y < 0) return std::log2((double)C);
        return -std::log2(std::max(1e-12, v[y] / sm));
    }
};

// ------------------------------------------------------------------ referencia
// Contador de orden C con retroceso. `fin` apunta al ULTIMO simbolo del contexto.
struct Ref {
    std::vector<std::map<std::string, std::map<int, int>>> ord;
    std::vector<std::map<std::string, int>> tot;
    std::map<int, int> uni;
    int C = 0, A = 256;
    double alfa = 1.0;

    void entrena(const std::vector<unsigned char> &t, int n) {
        ord.assign((size_t)C + 1, {});
        tot.assign((size_t)C + 1, {});
        for (int i = 0; i + 1 < n; ++i) uni[t[i]]++;
        for (int o = 1; o <= C; ++o)
            for (int i = 0; i + o < n; ++i) {
                const std::string k((const char *)&t[i], (size_t)o);
                ord[o][k][t[i + o]]++;
                tot[o][k]++;
            }
    }

    double p(int y, const unsigned char *fin, int o) const {
        if (o == 0) {
            double s = 0.0;
            for (auto &kv : uni) s += kv.second;
            auto it = uni.find(y);
            const double cnt = it == uni.end() ? 0.0 : (double)it->second;
            return (cnt + 0.1) / (s + 0.1 * A);
        }
        const std::string k((const char *)(fin - (o - 1)), (size_t)o);
        auto it = ord[o].find(k);
        const double baja = p(y, fin, o - 1);
        if (it == ord[o].end()) return baja;
        const double t_ = (double)tot[o].at(k);
        auto jt = it->second.find(y);
        const double cnt = jt == it->second.end() ? 0.0 : (double)jt->second;
        return (cnt + alfa * baja) / (t_ + alfa);
    }

    double evalua(const std::vector<unsigned char> &t, int desde) const {
        double bits = 0.0;
        int m = 0;
        for (size_t i = (size_t)desde; i + (size_t)C < t.size(); ++i) {
            bits += -std::log2(std::max(1e-12, p(t[i + C], &t[i + C - 1], C)));
            ++m;
        }
        return bits / std::max(1, m);
    }
};

int main(int argc, char **argv) {
    const int NTR = argc > 1 ? std::atoi(argv[1]) : 200000;
    const int NTE = argc > 2 ? std::atoi(argv[2]) : 20000;
    const int N   = argc > 3 ? std::atoi(argv[3]) : 2000;
    const double AL = argc > 4 ? std::atof(argv[4]) : 1.0;
    const double RH = argc > 5 ? std::atof(argv[5]) : 0.9;
    const unsigned SEED = argc > 6 ? (unsigned)std::atoi(argv[6]) : 1234u;
    const double LAM = argc > 7 ? std::atof(argv[7]) : 1.0;
    const double P0  = argc > 8 ? std::atof(argv[8]) : 10.0;
    const int CC     = argc > 9 ? std::atoi(argv[9]) : 0;
    // METODO 1 = RLS/FORCE (O(F^2), solo cabe con pocos rasgos);
    // METODO 0 = descenso del error (local, O(clases x rasgos)).
    const int METODO = argc > 10 ? std::atoi(argv[10]) : 0;
    const double TLR = argc > 11 ? std::atof(argv[11]) : 0.02;
    const int EPOCAS = argc > 12 ? std::atoi(argv[12]) : 2;
    const double DECAE = argc > 13 ? std::atof(argv[13]) : 0.0;
    const int GRADO = argc > 14 ? std::atoi(argv[14]) : 8;
    const double BETA = argc > 15 ? std::atof(argv[15]) : 0.0;  // simbolo anterior
    const int ADA = argc > 16 ? std::atoi(argv[16]) : 0;

    auto train = load("data/real_train.txt");
    auto test  = load("data/real_hold.txt");
    if ((int)train.size() < NTR - 1) {
        std::fprintf(stderr, "train tiene %zu, se pidieron %d\n", train.size(), NTR);
        return 1;
    }

    // vocabulario: solo los simbolos presentes en el train
    std::vector<int> visto(256, 0);
    for (int i = 0; i < NTR; ++i) visto[train[i]] = 1;
    std::vector<int> clases;
    for (int c = 0; c < 256; ++c) if (visto[c]) clases.push_back(c);
    const int A = (int)clases.size();

    const int F = N + CC;
    Lector lec;
    lec.init(F, clases, LAM, P0, METODO == 1);
    lec.tasa_grad = TLR;
    lec.decae = DECAE;
    lec.adagrad = ADA != 0;
    if (lec.adagrad) lec.ac.assign((size_t)lec.C * lec.F, 0.0);

    Reservorio res;
    // grado ANTES de init: init dimensiona las listas de vecinos con el.
    if (N > 0) res.init(N, 256, SEED, AL, RH, GRADO);
    if (N > 0) res.beta = BETA;

    std::printf("adagrad %d | ", lec.adagrad);
    std::printf("train %d | test %d | alfabeto %d | rasgos %d (reservorio %d + contexto %d)"
                " | alfa %.2f rho %.2f | metodo %s tasa %.3f seed %u\n",
                NTR, NTE, A, F, N, CC, AL, RH,
                METODO == 1 ? "RLS/FORCE" : "error local", TLR, SEED);

    // z: estado del reservorio (normalizado) + contexto directo one-hot
    std::vector<double> zc(CC * 256, 0.0);
    auto lee = [&](const std::vector<unsigned char> &txt, int n, bool aprender) {
        double bits = 0.0;
        int m = 0;
        if (N > 0) res.reset();
        std::fill(zc.begin(), zc.end(), 0.0);
        for (int t = 0; t + 1 < n; ++t) {
            if (N > 0) {
                res.paso(txt[t]);
                for (int i = 0; i < N; ++i) lec.z[i] = res.x[i];
            }
            // contexto directo: ultimos CC simbolos (el actual incluido)
            for (int c = 0; c < CC; ++c) {
                const int k = t - c;
                if (k < 0) continue;
                lec.z[N + c * 256 + txt[k]] = 1.0;
            }
            if (aprender) bits += lec.paso(txt[t + 1]);
            else          bits += lec.evaluar(txt[t + 1], 1.0);
            ++m;
            // limpiar solo lo que se encendio
            for (int c = 0; c < CC; ++c) {
                const int k = t - c;
                if (k < 0) continue;
                lec.z[N + c * 256 + txt[k]] = 0.0;
            }
        }
        return std::make_pair(bits / std::max(1, m), m);
    };

    // --- entrenamiento (2 pasadas: la segunda consolida) ---
    std::printf("entrenando (RLS local, en flujo)...\n");
    std::fflush(stdout);
    for (int ep = 0; ep < EPOCAS; ++ep) {
        auto r = lee(train, NTR, true);
        std::printf("  pasada %d: %.3f bpc (train)\n", ep + 1, r.first);
        std::fflush(stdout);
    }

    // --- test ---
    const int nte = std::min((int)test.size(), NTE);
    std::vector<unsigned char> te(test.begin(), test.begin() + nte);
    auto res_test = lee(te, nte, false);

    // --- referencias ---
    auto ref_de = [&](int c) {
        Ref r; r.C = c; r.A = A; r.entrena(train, NTR);
        return r.evalua(te, 0);
    };
    const double r1 = ref_de(1);
    const double r2 = ref_de(2);
    const double r3 = ref_de(3);
    const double rm = CC > 0 ? ref_de(CC) : -1.0;

    std::printf("\n--- RESULTADO ---\n");
    std::printf("  reservorio %d + lector local      : %.3f bpc\n", N, res_test.first);
    std::printf("  referencia 1 caracter             : %.3f bpc\n", r1);
    std::printf("  referencia 2 caracteres           : %.3f bpc\n", r2);
    std::printf("  referencia 3 caracteres           : %.3f bpc\n", r3);
    if (rm > 0.0) {
        std::printf("  referencia %d caracteres (misma C)  : %.3f bpc\n", CC, rm);
        std::printf("  diferencia frente a su referencia : %+.3f bpc (%s)\n",
                    res_test.first - rm, res_test.first < rm ? "GANA" : "PIERDE");
    }
    std::printf("  diferencia frente al BIGRAMA      : %+.3f bpc (%s)\n",
                res_test.first - r1, res_test.first < r1 ? "GANA" : "PIERDE");
    std::printf("  simbolos de test                  : %d\n", res_test.second);
    return 0;
}
