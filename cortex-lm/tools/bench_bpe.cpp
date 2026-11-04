// bench_bpe — mide sobre PIEZAS (trozos de palabra) en vez de letras.
//
// POR QUE
// -------
// Con letras, un contador de 3 caracteres es un rival durisimo y la red solo
// aporta 0,036 bpc. Con piezas (" que", "cion", "mente") el contexto util se
// reparte en mas simbolos: la red tiene sitio para aportar algo.
//
// QUE MIDE
// --------
//   1) contadores de contexto de 1, 2 y 3 piezas  (el suelo a batir)
//   2) reservorio + lector LOCAL sobre piezas
//   3) los dos combinados con peso adaptable (la comparacion honesta)
//
// Todo se mide en BITS POR BYTE de texto original, no por pieza: asi las cifras
// son comparables con todo lo anterior (que era por caracter == por byte).
//
// Uso:
//   bench_bpe N N ALFA RHO [SEED] [TASA] [PASADAS] [CP]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

// ------------------------------------------------------------------ lectura
static std::vector<uint32_t> carga_ids(const char *p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { std::fprintf(stderr, "no se puede abrir %s\n", p); std::exit(1); }
    std::vector<uint32_t> v;
    char b[4];
    while (in.read(b, 4)) {
        v.push_back((uint32_t)(unsigned char)b[0] |
                    ((uint32_t)(unsigned char)b[1] << 8) |
                    ((uint32_t)(unsigned char)b[2] << 16) |
                    ((uint32_t)(unsigned char)b[3] << 24));
    }
    return v;
}

static size_t bytes_de(const char *p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    return (size_t)in.tellg();
}

// --------------------------------------------------------------- reservorio
struct Reservorio {
    int N = 0, A = 0, grado = 8;
    std::vector<int> ix;
    std::vector<float> wv;
    std::vector<float> Win;      // A x N (entrada por simbolo)
    std::vector<float> x, xn;
    double rho = 0.9, alfa = 0.35;

    void init(int n, int alf, unsigned seed, double g, double r, int gr) {
        N = n; A = alf; alfa = g; rho = r;
        grado = std::min(gr, std::max(1, n));
        std::mt19937 rng(seed);
        std::normal_distribution<double> nd(0.0, 1.0);
        std::uniform_int_distribution<int> jd(0, n - 1);
        const double sw = 1.0 / std::sqrt((double)grado);
        ix.assign((size_t)n * grado, 0);
        wv.assign((size_t)n * grado, 0.0f);
        for (int i = 0; i < n; ++i)
            for (int k = 0; k < grado; ++k) {
                ix[(size_t)i * grado + k] = jd(rng);
                wv[(size_t)i * grado + k] = (float)(nd(rng) * sw);
            }
        const double si = 1.0 / std::sqrt((double)std::max(1, A));
        Win.assign((size_t)A * n, 0.0f);
        for (size_t i = 0; i < Win.size(); ++i) Win[i] = (float)(nd(rng) * si);
        x.assign(n, 0.0f);
        xn.assign(n, 0.0f);
    }
    void reset() { std::fill(x.begin(), x.end(), 0.0f); }
    void paso(uint32_t s) {
        const float *w = &Win[(size_t)s * N];
        const float r = (float)rho, g = (float)alfa;
        for (int i = 0; i < N; ++i) {
            const int *pi = &ix[(size_t)i * grado];
            const float *pv = &wv[(size_t)i * grado];
            float acc = 0.0f;
            for (int k = 0; k < grado; ++k) acc += pv[k] * x[pi[k]];
            x[i] = std::tanh(r * acc + g * w[i]);
        }
    }
};

// ------------------------------------------------------------------- lector
// Regla LOCAL: cada salida se corrige con SU error. O(clases x rasgos).
struct Lector {
    int F = 0, C = 0;
    std::vector<double> W, b, lg, z;
    std::vector<int> voc, pos;
    double tasa = 0.05;

    void init(int nf, const std::vector<int> &clases, double t) {
        F = nf; C = (int)clases.size(); voc = clases; tasa = t;
        pos.assign(1 << 20, -1);
        for (int i = 0; i < C; ++i) pos[voc[i]] = i;
        W.assign((size_t)C * F, 0.0);
        b.assign(C, 0.0);
        lg.assign(C, 0.0);
        z.assign(F, 0.0);
    }

    // probabilidad de la clase (indice) dada el z actual
    double prob_i(int y) {
        double mx = -1e30, sm = 0.0;
        for (int c = 0; c < C; ++c) {
            double a = b[c];
            const double *w = &W[(size_t)c * F];
            for (int k = 0; k < F; ++k) a += w[k] * z[k];
            lg[c] = a;
            if (a > mx) mx = a;
        }
        for (int c = 0; c < C; ++c) {
            lg[c] = std::exp(lg[c] - mx);
            sm += lg[c];
        }
        return std::max(1e-12, lg[y] / sm);
    }

    // bits del simbolo y (indice) y, si aprender, movimiento local
    int nneg = 40;          // clases al azar que tambien se corrigen
    std::mt19937 rng{1234};

    double paso(int y, bool aprender) {
        const double p = prob_i(y);
        if (false && aprender && nneg < C - 1) {   // descartado: 5,004 bits/byte
            double sm = 0.0;
            for (int c = 0; c < C; ++c) sm += lg[c];
            // clase correcta
            {
                const double g = lg[y] / sm - 1.0;
                double *w = &W[(size_t)y * F];
                for (int k = 0; k < F; ++k) w[k] -= tasa * g * z[k];
                b[y] -= tasa * g;
            }
            // negativos al azar (probabilidad proporcional al error: se elige al
            // azar y se corrige con SU error, igual que antes)
            std::uniform_int_distribution<int> jd(0, C - 1);
            for (int s_ = 0; s_ < nneg; ++s_) {
                const int c = jd(rng);
                if (c == y) continue;
                const double g = lg[c] / sm;
                if (g < 1e-9) continue;
                double *w = &W[(size_t)c * F];
                for (int k = 0; k < F; ++k) w[k] -= tasa * g * z[k];
                b[c] -= tasa * g;
            }
            return -std::log2(p);
        }
        if (aprender) {
            // recomputar el softmax ya esta en lg (normalizado)
            double sm = 0.0;
            for (int c = 0; c < C; ++c) sm += lg[c];
            for (int c = 0; c < C; ++c) {
                const double g = lg[c] / sm - (c == y ? 1.0 : 0.0);
                if (g == 0.0) continue;
                double *w = &W[(size_t)c * F];
                for (int k = 0; k < F; ++k) w[k] -= tasa * g * z[k];
                b[c] -= tasa * g;
            }
        }
        return -std::log2(p);
    }
};

// ---------------------------------------------------------------- contador
struct Ppm {
    int C = 0, A = 0;
    double alfa = 1.0;
    double minveces = 3.0;
    std::vector<std::map<std::vector<uint32_t>, std::map<uint32_t, int>>> ord;
    std::vector<std::map<std::vector<uint32_t>, int>> tot;
    std::map<uint32_t, int> uni;

    void entrena(const std::vector<uint32_t> &t) {
        ord.assign((size_t)C + 1, {});
        tot.assign((size_t)C + 1, {});
        for (size_t i = 0; i + 1 < t.size(); ++i) uni[t[i]]++;
        for (int o = 1; o <= C; ++o)
            for (size_t i = 0; i + o < t.size(); ++i) {
                std::vector<uint32_t> k(t.begin() + i, t.begin() + i + o);
                ord[o][k][t[i + o]]++;
                tot[o][k]++;
            }
    }

    double p(uint32_t y, const std::vector<uint32_t> &ctx, int o) const {
        if (o == 0) {
            double s = 0.0;
            for (auto &kv : uni) s += kv.second;
            auto it = uni.find(y);
            const double cnt = it == uni.end() ? 0.0 : (double)it->second;
            return (cnt + 0.1) / (s + 0.1 * A);
        }
        const std::vector<uint32_t> k(ctx.end() - o, ctx.end());
        auto it = ord[o].find(k);
        const double baja = p(y, ctx, o - 1);
        if (it == ord[o].end()) return baja;
        const double t_ = (double)tot[o].at(k);
        // con menos de MINVECES observaciones no se confia en el contexto:
        // se usa el alfa grande para que pese poco frente al retroceso.
        if (t_ < minveces) return (0.0 + 10.0 * baja) / (t_ + 10.0);
        auto jt = it->second.find(y);
        const double cnt = jt == it->second.end() ? 0.0 : (double)jt->second;
        return (cnt + alfa * baja) / (t_ + alfa);
    }
};

int main(int argc, char **argv) {
    const int NTR = argc > 1 ? std::atoi(argv[1]) : 60000;
    const int NN  = argc > 2 ? std::atoi(argv[2]) : 1500;
    const double AL = argc > 3 ? std::atof(argv[3]) : 0.35;
    const double RH = argc > 4 ? std::atof(argv[4]) : 0.9;
    const unsigned SEED = argc > 5 ? (unsigned)std::atoi(argv[5]) : 1234u;
    const double TASA = argc > 6 ? std::atof(argv[6]) : 0.05;
    const int EPOCAS  = argc > 7 ? std::atoi(argv[7]) : 2;
    const int CP      = argc > 8 ? std::atoi(argv[8]) : 3;

    auto train = carga_ids("data/train.ids");
    auto test  = carga_ids("data/hold.ids");
    if ((int)train.size() < NTR + 2) {
        std::fprintf(stderr, "train tiene %zu piezas, se pidieron %d\n",
                     train.size(), NTR);
        return 1;
    }
    const size_t bytes_tr = bytes_de("data/real_train.txt");
    const size_t bytes_te = bytes_de("data/real_hold.txt");
    (void)bytes_tr;

    // alfabeto real
    std::vector<int> visto(1 << 20, 0);
    for (int i = 0; i < NTR; ++i) visto[train[i]] = 1;
    std::vector<int> clases;
    for (int c = 0; c < (1 << 20); ++c) if (visto[c]) clases.push_back(c);
    const int A = (int)clases.size();
    std::printf("piezas: train %d | test %zu | alfabeto %d | alfa %.2f rho %.2f "
                "N %d tasa %.3f\n", NTR, test.size(), A, AL, RH, NN, TASA);

    // ---- reservorio + lector ----
    Reservorio res;
    res.init(NN, A, SEED, AL, RH, 8);
    // los simbolos son arbitrarios (hasta 1<<20): se mapean a 0..A-1
    std::vector<int> idx_de(1 << 20, -1);
    for (int i = 0; i < A; ++i) idx_de[clases[i]] = i;

    // La entrada del reservorio admite A simbolos; se usa el indice de clase.
    Lector lec;
    lec.init(NN, clases, TASA);

    auto lee = [&](const std::vector<uint32_t> &txt, int n, bool aprender,
                   std::vector<float> *bits_pieza) {
        double bits = 0.0;
        int m = 0;
        res.reset();
        for (int t = 0; t + 1 < n; ++t) {
            const int xs = idx_de[txt[t]];
            const int ys = idx_de[txt[t + 1]];
            if (ys < 0) {             // pieza que no estaba en el train
                bits += 16.0; ++m;
                if (bits_pieza) bits_pieza->push_back(16.0f);
                continue;
            }
            res.paso((uint32_t)(xs < 0 ? 0 : xs));
            for (int i = 0; i < NN; ++i) lec.z[i] = res.x[i];
            const int y = ys;
            const double br = lec.paso(y, aprender);
            bits += br;
            if (bits_pieza) bits_pieza->push_back((float)br);
            ++m;
        }
        return bits / std::max(1, m);
    };

    std::printf("entrenando reservorio + lector local...\n");
    for (int ep = 0; ep < EPOCAS; ++ep) {
        const double b = lee(train, NTR, true, nullptr);
        std::printf("  pasada %d: %.3f bits/pieza (train)\n", ep + 1, b);
        std::fflush(stdout);
    }

    const int nte = (int)test.size();
    std::vector<float> bits_red;
    bits_red.reserve(nte);
    const double b_pieza = lee(test, nte, false, &bits_red);
    const double b_byte = b_pieza * (double)(nte - 1) / (double)bytes_te;

    // ---- contadores ----
    std::printf("\n--- suelo: contadores de contexto (sobre piezas) ---\n");
    double suelo[4] = {0, 0, 0, 0};
    for (int o = 1; o <= 3; ++o) {
        Ppm pp; pp.C = o; pp.A = A; pp.entrena(train);
        double bits = 0.0;
        std::vector<uint32_t> ctx;
        for (int t = 0; t + 1 < nte; ++t) {
            ctx.assign(test.begin(), test.begin() + t + 1);
            const double q = pp.p(test[t + 1], ctx, o);
            bits += -std::log2(std::max(1e-12, q));
        }
        suelo[o] = bits * (nte - 1) / (double)bytes_te / (nte - 1);
        suelo[o] = bits / (double)bytes_te;   // bits por BYTE de texto
        std::printf("  contador %d piezas: %.3f bits/byte\n", o, suelo[o]);
        std::fflush(stdout);
    }

    std::printf("\n--- RESULTADO (bits por BYTE de texto original) ---\n");
    std::printf("  reservorio + lector local : %.3f bits/byte (%.3f bits/pieza)\n",
                b_byte, b_pieza);
    for (int o = 1; o <= 3; ++o)
        std::printf("  contador %d piezas        : %.3f bits/byte\n", o, suelo[o]);
    std::printf("  simbolos de test          : %d piezas / %zu bytes\n",
                nte - 1, bytes_te);
    return 0;
}
