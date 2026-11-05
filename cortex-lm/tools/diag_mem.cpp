// diag_mem — cuanta MEMORIA lleva el estado, medido como capacidad de leer el
// pasado desde el estado actual.
//
// Para cada caracter c se calcula el estado medio mu_c (centroide). Luego, dado
// el estado en el instante t, se busca el centroide mas cercano y se comprueba
// si acierta:
//
//   acierto(char t)    -> informacion que el estado tiene de la letra ACTUAL
//   acierto(char t-1)  -> informacion que conserva de la ANTERIOR (memoria)
//   acierto(char t-4)  -> memoria un poco mas larga
//
// Si acierta la actual casi siempre y la anterior nunca, la red es una foto del
// alfabeto y su techo es el bigrama. Ademas se mide la entropia condicionada:
// H(char t | estado) estimada por el error del clasificador de centroides, para
// tener la cuenta en bits/caracter y poder compararla con los contadores.
//
// Uso: diag_mem CORPUS [N] [SEED]

#include "cortex_lm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace cortex;

static std::vector<unsigned char> carga(const char *p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { std::fprintf(stderr, "no se puede abrir %s\n", p); std::exit(1); }
    std::vector<unsigned char> v;
    char c;
    while (in.get(c)) v.push_back((unsigned char)c);
    return v;
}

// acierto top-1 con centroides, para predecir el caracter de hace `lag`.
// ATENCION: los centroides se ajustan en la PRIMERA mitad y se prueban en la
// SEGUNDA. Sin esa separacion el resultado es memorizacion: con 2048 dimensiones
// y 1200 muestras el vecino mas cercano siempre acierta y la medida no vale.
static void acierta(const std::vector<std::vector<float>> &st,
                    const std::vector<unsigned char> &txt, int lag,
                    double *bits_cond) {
    const int T = (int)st.size();
    const int corte = T / 2;
    std::map<int, std::vector<double>> sum;
    std::map<int, int> cnt;
    const size_t D = st[0].size();
    for (int t = lag; t < corte; ++t) {
        const int c = txt[t - lag];
        auto &s = sum[c];
        if (s.empty()) s.assign(D, 0.0);
        for (size_t i = 0; i < D; ++i) s[i] += st[t][i];
        cnt[c]++;
    }
    for (auto &kv : sum) {
        const double n = std::max(1, cnt[kv.first]);
        for (double &x : kv.second) x /= n;
    }
    // evaluar de verdad: se usan los centroides de TODOS los datos (estimacion
    // optimista, pero igual para todos los casos comparados aqui)
    long ok = 0;
    double ll = 0.0;
    long tot = 0;
    for (int t = corte; t < T; ++t) {
        const int real = txt[t - lag];
        int mejor = -1;
        double dmin = 1e30;
        std::vector<double> dist;
        if (sum.find(real) == sum.end()) continue;   // clase no vista al ajustar
        for (auto &kv : sum) {
            double ac = 0.0;
            const auto &mu = kv.second;
            for (size_t i = 0; i < D; ++i) { const double d = st[t][i] - mu[i]; ac += d * d; }
            dist.push_back(ac);
            if (ac < dmin) { dmin = ac; mejor = kv.first; }
        }
        if (mejor == real) ++ok;
        // softmax invertido sobre las distancias -> probabilidad estimada
        double Z = 0.0;
        std::vector<double> pr;
        for (double d : dist) { const double e = std::exp(-d / (dmin + 1e-9)); pr.push_back(e); Z += e; }
        // buscar la posicion del real: se guarda aparte porque el mapa ordena
        double p_real = 1e-9;
        {
            int k = 0;
            for (auto &kv : sum) { if (kv.first == real) p_real = std::max(1e-9, pr[k]); ++k; }
        }
        ll += -std::log2(p_real / std::max(1e-12, Z));
        ++tot;
    }
    *bits_cond = ll / std::max<long>(1, tot);
    std::printf("   lag %d: acierto %5.1f%%\n", lag, 100.0 * ok / std::max<long>(1, tot));
}

static void mide(Params p, const char *nombre,
                 const std::vector<unsigned char> &txt, unsigned seed) {
    Engine e(p);
    e.build_random(seed);
    std::vector<float> f;
    std::vector<std::vector<float>> st;
    std::vector<unsigned char> etiquetas;
    for (unsigned char c : txt) {
        e.feed(c);
        e.full_features(f);           // lo mismo que ve el lector por defecto
        st.emplace_back(f.begin(), f.end());
        etiquetas.push_back(c);
    }
    std::printf("%-12s estados: 1 de cada caracter\n", nombre);
    for (int lag : {0, 1, 2, 4}) {
        double bits = 0.0;
        acierta(st, etiquetas, lag, &bits);
    }
    std::fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "uso: diag_mem CORPUS [N] [SEED]\n"); return 1; }
    auto corpus = carga(argv[1]);
    const int N = argc > 2 ? std::atoi(argv[2]) : 1500;
    const unsigned SEED = argc > 3 ? (unsigned)std::atoi(argv[3]) : 1234u;
    std::vector<unsigned char> txt(corpus.begin(),
                                   corpus.begin() + std::min((size_t)N, corpus.size()));
    Params base;
    base.seed = SEED;
    base.layer_size = 1024;
    base.ticks_per_char = 8;
    mide(base, "BASE", txt, SEED);
    { Params p = base; p.ticks_per_char = 24; mide(p, "TICKS 24", txt, SEED); }
    { Params p = base; p.w_scale = 0.6f; mide(p, "RECURR 0.6", txt, SEED); }
    { Params p = base; p.inh_gain = 0.005f; mide(p, "INH 0.005", txt, SEED); }
    return 0;
}
