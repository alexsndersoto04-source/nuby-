// escribir.cpp — pone el sistema a ESCRIBIR texto, que es lo unico que importa.
//
// POR QUE
// -------
// Llevamos muchas medidas de "bits por caracter", y eso no dice si el texto se
// parece al espanol. Aqui se genera texto y se compara a ojo. Es la prueba que
// el usuario puede juzgar sin saber nada de numeros.
//
// COMO
// ----
// 1) Se entrena el reservorio + lector local como siempre.
// 2) Se le da un arranque de texto (los ultimos caracteres del train).
// 3) Se predice el siguiente caracter: se muestrea de la distribucion que da el
//    modelo (no el mas probable, que se queda en bucle).
// 4) Se escribe lo generado.
//
// Uso:
//   escribir SEMILLA [CARACTERES] [TEMPERATURA]

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

static std::vector<unsigned char> load(const char *p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { std::fprintf(stderr, "no se puede abrir %s\n", p); std::exit(1); }
    std::vector<unsigned char> v;
    char c;
    while (in.get(c)) v.push_back((unsigned char)c);
    return v;
}

struct Reservorio {
    int N = 0, A = 0, grado = 8;
    std::vector<int> ix;
    std::vector<float> wv, Win, x;
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

struct Lector {
    int F = 0, C = 0;
    std::vector<double> W, b, lg, z, pr;
    std::vector<int> voc, pos;
    double tasa = 0.05;

    void init(int nf, const std::vector<int> &clases, double t) {
        F = nf; C = (int)clases.size(); voc = clases; tasa = t;
        pos.assign(256, -1);
        for (int i = 0; i < C; ++i) pos[voc[i]] = i;
        W.assign((size_t)C * F, 0.0);
        b.assign(C, 0.0);
        lg.assign(C, 0.0);
        pr.assign(C, 0.0);
        z.assign(F, 0.0);
    }

    // devuelve probabilidades en pr[] y los bits del simbolo y
    double paso(int y, bool aprender) {
        double mx = -1e30, sm = 0.0;
        for (int c = 0; c < C; ++c) {
            double a = b[c];
            const double *w = &W[(size_t)c * F];
            for (int k = 0; k < F; ++k) a += w[k] * z[k];
            lg[c] = a;
            if (a > mx) mx = a;
        }
        for (int c = 0; c < C; ++c) { lg[c] = std::exp(lg[c] - mx); sm += lg[c]; }
        for (int c = 0; c < C; ++c) pr[c] = lg[c] / sm;
        const double p = std::max(1e-12, pr[y]);
        if (aprender) {
            for (int c = 0; c < C; ++c) {
                const double g = pr[c] - (c == y ? 1.0 : 0.0);
                if (g == 0.0) continue;
                double *w = &W[(size_t)c * F];
                for (int k = 0; k < F; ++k) w[k] -= tasa * g * z[k];
                b[c] -= tasa * g;
            }
        }
        return -std::log2(p);
    }
};

int main(int argc, char **argv) {
    const unsigned SEM = argc > 1 ? (unsigned)std::atoi(argv[1]) : 1234u;
    const int NGEN   = argc > 2 ? std::atoi(argv[2]) : 400;
    const double T   = argc > 3 ? std::atof(argv[3]) : 1.0;
    const int NN = 1500, EPOCAS = 4;

    auto train = load("data/real_train.txt");
    std::vector<int> visto(256, 0);
    for (unsigned char c : train) visto[c] = 1;
    std::vector<int> clases;
    for (int c = 0; c < 256; ++c) if (visto[c]) clases.push_back(c);

    Reservorio res;
    res.init(NN, 256, SEM, 0.35, 0.9, 8);
    Lector lec;
    lec.init(NN, clases, 0.05);

    std::printf("entrenando (%zu caracteres, %zu clases)...\n", train.size(),
                clases.size());
    std::fflush(stdout);
    for (int ep = 0; ep < EPOCAS; ++ep) {
        double bits = 0.0;
        int m = 0;
        res.reset();
        for (size_t t = 0; t + 1 < train.size(); ++t) {
            res.paso(train[t]);
            for (int i = 0; i < NN; ++i) lec.z[i] = res.x[i];
            bits += lec.paso(lec.pos[train[t + 1]], true);
            ++m;
        }
        std::printf("  pasada %d: %.2f bits/caracter\n", ep + 1, bits / m);
        std::fflush(stdout);
    }

    // arranque: los ultimos 40 caracteres del train
    const size_t arranque = 40;
    std::printf("\n--- TEXTO GENERADO (semilla %u, temperatura %.2f) ---\n",
                SEM, T);
    std::mt19937 rng(SEM * 31u + 7u);
    res.reset();
    for (size_t t = train.size() - arranque; t + 1 < train.size(); ++t)
        res.paso(train[t]);
    std::string salida;
    int ultimo = train.back();
    std::uniform_real_distribution<double> ud(0.0, 1.0);
    for (int n = 0; n < NGEN; ++n) {
        res.paso((uint32_t)ultimo);
        for (int i = 0; i < NN; ++i) lec.z[i] = res.x[i];
        lec.paso(0, false);          // rellena pr[] sin aprender
        // muestreo con temperatura
        double suma = 0.0;
        std::vector<double> peso(lec.C);
        for (int c = 0; c < lec.C; ++c) {
            peso[c] = std::pow(std::max(1e-12, lec.pr[c]), 1.0 / T);
            suma += peso[c];
        }
        double r = ud(rng) * suma, acc = 0.0;
        int elegido = lec.C - 1;
        for (int c = 0; c < lec.C; ++c) {
            acc += peso[c];
            if (r <= acc) { elegido = c; break; }
        }
        ultimo = lec.voc[elegido];
        salida.push_back((char)ultimo);
    }
    std::printf("%s\n", salida.c_str());
    std::printf("--- fin ---\n");
    return 0;
}
