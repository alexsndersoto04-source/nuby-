// Entrenamiento CONJUNTO en linea: lector y plasticidad recurrente a la vez.
//
// POR QUE
// -------
// `bench.cpp` entrena la red en una sola pasada y el lector despues. Asi la
// plasticidad solo ve la R_ inicial (aleatoria), su senal de aprendizaje vale
// casi cero y la prueba no mide nada: el resultado sale identico al de no
// tener plasticidad.
//
// Aqui ambos aprenden a la vez, simbolo a simbolo, que es como funcionaria el
// sistema real y la unica forma justa de medir la regla local.
//
// Uso: bench_joint EPOCHS K LH PLASTIC ETA WSCALE LAG
#include "cortex_lm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <vector>

using namespace cortex;

static std::vector<unsigned char> load(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "no se puede abrir %s\n", path);
        std::exit(1);
    }
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    const int   EPOCH = argc > 1 ? std::atoi(argv[1]) : 8;
    const int   K     = argc > 2 ? std::atoi(argv[2]) : 64;
    const int   LH    = argc > 3 ? std::atoi(argv[3]) : 8;
    const int   PL    = argc > 4 ? std::atoi(argv[4]) : 0;
    const float ETA   = argc > 5 ? (float)std::atof(argv[5]) : 0.0f;
    const float WSC   = argc > 6 ? (float)std::atof(argv[6]) : -1.0f;
    const int   LAG   = argc > 7 ? std::atoi(argv[7]) : 1;
    const int   NMAX  = argc > 8 ? std::atoi(argv[8]) : 0;   // 0 = todo el train

    Params p;
    if (ETA > 0) p.eta = ETA;
    if (WSC >= 0) p.w_scale = WSC;
    Engine e(p);
    e.build_random(p.seed);

    auto train = load("data/spanish_small.txt");
    auto test  = load("data/spanish_holdout.txt");
    const int Dd = p.layer_size;
    const int NF = K * LH;
    int NTR = (int)train.size() - std::max(1, LAG) - 1;
    if (NMAX > 0 && NMAX < NTR) NTR = NMAX;

    // --- unica pasada de calibracion para elegir dimensiones y escalar ---
    // (se hace antes de aprender, sobre la red inicial: no filtra informacion
    //  del test, solo normaliza)
    std::vector<float> f;
    std::vector<std::vector<float>> S(NTR);
    e.reset_state();
    for (int t = 0; t < NTR; ++t) {
        e.feed(train[t]);
        e.theta_features(f);
        S[t] = f;
    }
    std::vector<double> mean(Dd, 0.0);
    for (int j = 0; j < Dd; ++j) {
        double a = 0;
        for (int t = 0; t < NTR; ++t) a += S[t][j];
        mean[j] = a / NTR;
    }
    std::vector<std::pair<double, int>> var(Dd);
    for (int j = 0; j < Dd; ++j) {
        double v = 0;
        for (int t = 0; t < NTR; ++t) {
            const double d = S[t][j] - mean[j];
            v += d * d;
        }
        var[j] = {v / NTR, j};
    }
    std::sort(var.rbegin(), var.rend());
    std::vector<int> sel(K);
    for (int k = 0; k < K; ++k) sel[k] = var[k].second;
    std::vector<double> mu(K, 0.0), sd(K, 0.0);
    for (int k = 0; k < K; ++k) {
        double a = 0;
        for (int t = 0; t < NTR; ++t) a += S[t][sel[k]];
        mu[k] = a / NTR;
        double v = 0;
        for (int t = 0; t < NTR; ++t) {
            const double d = S[t][sel[k]] - mu[k];
            v += d * d;
        }
        sd[k] = std::sqrt(v / NTR) + 1.0;
    }
    std::vector<std::vector<float>>().swap(S);   // liberar

    // --- lector softmax propio de este banco ---
    std::vector<double> W((size_t)256 * NF, 0.0), b(256, 0.0), lg(256);
    const double lr = 0.05;

    std::vector<std::vector<float>> ring(LH, std::vector<float>(Dd, 0.0f));
    std::vector<double> z(NF);
    auto snapshot = [&](size_t t) {
        for (int h = LH - 1; h > 0; --h) ring[h] = ring[h - 1];
        ring[0] = f;
        (void)t;
    };
    auto buildz = [&]() {
        for (int h = 0; h < LH; ++h)
            for (int k = 0; k < K; ++k) z[h * K + k] = (ring[h][sel[k]] - mu[k]) / sd[k];
    };

    // --- entrenamiento conjunto, en linea ---
    e.reset_state();
    for (int ep = 0; ep < EPOCH; ++ep) {
        for (int h = 0; h < LH; ++h) std::fill(ring[h].begin(), ring[h].end(), 0.0f);
        e.reset_state();
        const long sp0 = e.spikes(), tk0 = e.ticks();
        for (int t = 0; t < NTR; ++t) {
            e.feed(train[t]);
            e.theta_features(f);
            snapshot((size_t)t);
            buildz();
            const int y = train[t + LAG];

            // paso del lector
            double mx = -1e30;
            for (int c = 0; c < 256; ++c) {
                double v = b[c];
                const double* r = &W[(size_t)c * NF];
                for (int k = 0; k < NF; ++k) v += r[k] * z[k];
                lg[c] = v;
                if (v > mx) mx = v;
            }
            double s = 0;
            for (int c = 0; c < 256; ++c) { lg[c] = std::exp(lg[c] - mx); s += lg[c]; }
            std::vector<float> err(256, 0.0f);
            for (int c = 0; c < 256; ++c) {
                const double g = lg[c] / s - (c == y ? 1.0 : 0.0);
                err[c] = (float)g;
                if (g == 0.0) continue;
                b[c] -= lr * g;
                double* r = &W[(size_t)c * NF];
                for (int k = 0; k < NF; ++k) r[k] -= lr * (g * z[k] + 1e-4 * r[k]);
            }
            // paso local de la red (usa R_ ya entrenada en las epocas previas)
            if (PL) e.learn(err.data(), f);
        }
        // tasa POR EPOCA (el contador de la red es acumulado, no por epoca)
        std::printf("  epoca %2d: %.1f disp/simbolo\n", ep + 1,
                    (double)(e.spikes() - sp0) / std::max(1.0, (double)(e.ticks() - tk0) / p.ticks_per_char));
        std::fflush(stdout);
    }

    // --- test sobre texto nunca visto ---
    std::map<std::pair<int, int>, int> bi;
    std::map<int, int> uni;
    for (int t = 0; t + 1 < NTR; ++t) { bi[{train[t], train[t + 1]}]++; uni[train[t]]++; }

    for (int h = 0; h < LH; ++h) std::fill(ring[h].begin(), ring[h].end(), 0.0f);
    e.reset_state();
    double bits = 0, bitsBigram = 0;
    int n = 0, cor = 0;
    for (size_t t = 0; t + std::max(1, LAG) < test.size(); ++t) {
        e.feed(test[t]);
        e.theta_features(f);
        snapshot(t);
        buildz();
        const int y = test[t + LAG];
        {
            auto it = bi.find({test[t], test[t + 1]});
            const double c = it == bi.end() ? 0.0 : it->second;
            bitsBigram += -std::log2((c + 0.1) / (uni[test[t]] + 0.1 * 256));
        }
        double mx = -1e30;
        int bst = 0;
        for (int c = 0; c < 256; ++c) {
            double v = b[c];
            const double* r = &W[(size_t)c * NF];
            for (int k = 0; k < NF; ++k) v += r[k] * z[k];
            lg[c] = v;
            if (v > mx) { mx = v; bst = c; }
        }
        double s = 0;
        for (int c = 0; c < 256; ++c) { lg[c] = std::exp(lg[c] - mx); s += lg[c]; }
        bits += -std::log2(std::max(1e-12, lg[y] / s));
        if (bst == y) cor++;
        n++;
    }
    std::printf("PLASTIC=%d eta=%.1e w_scale=%.2f K=%d LH=%d LAG=%d ep=%d\n",
                PL, ETA, p.w_scale, K, LH, LAG, EPOCH);
    std::printf("  TEST (no visto, %d simbolos): %.3f bpc | acierto %.1f %%\n",
                n, bits / n, 100.0 * cor / n);
    std::printf("  bigrama de referencia       : %.3f bpc\n", bitsBigram / n);
    return 0;
}
