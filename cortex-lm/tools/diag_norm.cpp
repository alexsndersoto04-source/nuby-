// diag_norm — reproduce EXACTLY the reader's normalization and report which
// dimensions survive, split by fast vs slow population.
//
// Why this exists: the bank's reader (tools/bench_bio.cpp, Lector::congela)
// kills any dimension whose standard deviation is below
//     piso = 0.05 * max_sd
// by setting its scale to 0. The slow population (n_fast..layer_size-1) IS the
// memory we built. If those dimensions have low variance, the memory reaches
// the reader dead, which explains why lag1 ~44% (diag_mem sees it) and holdout
// is identical to BASE (the reader never sees it).
//
// This tool replays the calibration of the reader over the first NCAL chars and
// prints, per population, how many dimensions survive.
//
// Uso: diag_norm CORPUS [N_CAL] [SEED] [n_fast] [tau_m_fast] [tau_m_slow] [slow_in]

#include "cortex_lm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "uso: diag_norm CORPUS [N_CAL] [SEED] [n_fast] [tau_m_fast] "
            "[tau_m_slow] [slow_in]\n");
        return 1;
    }
    auto corpus = carga(argv[1]);
    const int N_CAL = argc > 2 ? std::atoi(argv[2]) : 1500;
    const unsigned SEED = argc > 3 ? (unsigned)std::atoi(argv[3]) : 1234u;
    const int NFAST = argc > 4 ? std::atoi(argv[4]) : 0;
    const float T_F = argc > 5 ? (float)std::atof(argv[5]) : 8.0f;
    const float T_S = argc > 6 ? (float)std::atof(argv[6]) : 8.0f;
    const float SIG = argc > 7 ? (float)std::atof(argv[7]) : 0.0f;

    Params p;
    p.seed = SEED;
    p.layer_size = 1024;
    p.ticks_per_char = 8;
    p.n_fast = NFAST;
    p.tau_m_fast = T_F;
    p.tau_m_slow = T_S;
    p.slow_in_gain = SIG;

    Engine e(p);
    e.build_random(SEED);

    const int N = p.n_layers * p.layer_size;   // 1024
    const int F = 2 * N;                       // estado + fatiga (g_feat=2)

    // --- replay the reader's calibration (Lector::calibra) ---
    std::vector<double> mu(F, 0.0), sd(F, 0.0);
    std::vector<float> f;
    const int nc = std::min<int>(N_CAL, (int)corpus.size());
    for (int i = 0; i < nc; ++i) {
        e.feed(corpus[i]);
        e.full_features(f);
        const double n = (double)(i + 1);
        for (int k = 0; k < F && k < (int)f.size(); ++k) {
            const double d = (double)f[k] - mu[k];
            mu[k] += d / n;
            sd[k] += d * ((double)f[k] - mu[k]);
        }
    }

    // --- Lector::congela ---
    std::vector<double> s(F, 0.0);
    double mx = 0.0;
    for (int k = 0; k < F; ++k) {
        s[k] = std::sqrt(std::max(0.0, sd[k]) / std::max(1.0, (double)nc));
        mx = std::max(mx, s[k]);
    }
    const double piso = 0.05 * mx;

    char nombre[64];
    std::snprintf(nombre, sizeof(nombre), "n_fast=%d", NFAST);
    std::printf("%-12s calibrado sobre %d chars | max_sd %.6f | piso(5%%) %.6f\n",
                nombre, nc, mx, piso);

    // Cuatro poblaciones: estado rapido, estado lento, fatiga rapida, fatiga lenta.
    struct Bloque { const char *n; int a, b; };
    const Bloque blk[4] = {
        {"estado rapido",   0,      NFAST},
        {"estado lento",    NFAST,  N},
        {"fatiga rapida",   N,      N + NFAST},
        {"fatiga lento",    N + NFAST, 2 * N},
    };
    for (const auto &B : blk) {
        if (B.b <= B.a) { std::printf("  %-14s (vacio)\n", B.n); continue; }
        int vivos = 0;
        double smean = 0.0, smax = 0.0;
        for (int k = B.a; k < B.b; ++k) {
            smean += s[k];
            smax = std::max(smax, s[k]);
            if (s[k] >= piso && s[k] > 1e-9) ++vivos;
        }
        const int tot = B.b - B.a;
        std::printf("  %-14s %3d/%-4d sobreviven (%5.1f%%) | sd media %.6f | sd max %.6f\n",
                    B.n, vivos, tot, 100.0 * vivos / tot, smean / tot, smax);
    }
    std::printf("  -> cada dim con sd < piso se PONE a cero antes de que el lector la vea.\n");
    return 0;
}
