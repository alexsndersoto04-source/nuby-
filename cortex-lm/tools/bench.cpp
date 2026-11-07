// Banco de medida con protocolo honesto.
//
// POR QUE EXISTE
// --------------
// Todas las cifras anteriores salian de partir UN MISMO texto repetido en
// "train" y "test". El corpus de verificacion repite el Quijote en bucle, asi
// que el test era casi identico al train y el BPC medido era memorizacion.
// Aqui el test sale de `data/spanish_holdout.txt`, que no comparte ni una
// variante de superficie con el entrenamiento.
//
// QUE MIDE
// --------
// La red ve el texto de entrenamiento. El lector softmax (lo unico que
// aprende por gradiente) se entrena sobre las features de la red. El test se
// pasa por la red CONGELADA y se mide el BPC sobre texto nunca visto.
//
// Uso: bench NTR EPOCHS K LH [PLASTIC ETA WSCALE]
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
    const int   NTR   = argc > 1 ? std::atoi(argv[1]) : 8000;
    const int   EPOCH = argc > 2 ? std::atoi(argv[2]) : 12;
    const int   K     = argc > 3 ? std::atoi(argv[3]) : 64;
    const int   LH    = argc > 4 ? std::atoi(argv[4]) : 8;
    const int   PL    = argc > 5 ? std::atoi(argv[5]) : 0;
    const float ETA   = argc > 6 ? (float)std::atof(argv[6]) : 0.0f;
    const float WSC   = argc > 7 ? (float)std::atof(argv[7]) : -1.0f;
    const int   LAG   = argc > 8 ? std::atoi(argv[8]) : 1;   // 1 = siguiente simbolo, 0 = actual

    Params p;
    p.eta = ETA > 0 ? ETA : p.eta;
    if (WSC >= 0) p.w_scale = WSC;
    Engine e(p);
    e.build_random(p.seed);

    auto train = load("data/spanish_small.txt");
    auto test  = load("data/spanish_holdout.txt");
    const int Dd = p.layer_size;
    const int NF = K * LH;

    // --- calibracion: una pasada sin plasticidad para elegir dims y escalar ---
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

    // --- pasada 2: features con (o sin) plasticidad local ---
    e.reset_state();
    std::vector<std::vector<float>> SS(NTR);
    for (int t = 0; t < NTR; ++t) {
        e.feed(train[t]);
        e.theta_features(f);
        SS[t] = f;
        if (PL) {
            // Error difundido por la matriz fija B. En el uso real sale del
            // lector; aqui usamos el error de un bigrama para aislar el efecto
            // de la plasticidad sobre la representacion.
            std::vector<float> err(256, 0.0f);
            err[train[t + LAG]] = -1.0f;
            err[train[t]] = 1.0f;
            e.learn(err.data(), f);
        }
    }

    // desplazar en el tiempo para construir la ventana de LH estados
    std::vector<std::vector<float>> Z(NTR);
    for (int t = 0; t < NTR; ++t) {
        Z[t].resize(NF);
        for (int h = 0; h < LH; ++h) {
            const int s = t - h < 0 ? 0 : t - h;
            for (int k = 0; k < K; ++k) Z[t][h * K + k] = (SS[s][sel[k]] - mu[k]) / sd[k];
        }
    }

    // --- lector softmax multinomial (unico componente que usa gradiente) ---
    std::vector<double> W((size_t)256 * NF, 0.0), b(256, 0.0), lg(256);
    const double lr = 0.05;
    for (int ep = 0; ep < EPOCH; ++ep)
        for (int t = 0; t + std::max(1, LAG) < NTR; ++t) {
            const int y = train[t + LAG];
            double mx = -1e30;
            for (int c = 0; c < 256; ++c) {
                double v = b[c];
                const double* r = &W[(size_t)c * NF];
                for (int k = 0; k < NF; ++k) v += r[k] * Z[t][k];
                lg[c] = v;
                if (v > mx) mx = v;
            }
            double s = 0;
            for (int c = 0; c < 256; ++c) { lg[c] = std::exp(lg[c] - mx); s += lg[c]; }
            for (int c = 0; c < 256; ++c) {
                const double g = lg[c] / s - (c == y ? 1.0 : 0.0);
                if (g == 0.0) continue;
                b[c] -= lr * g;
                double* r = &W[(size_t)c * NF];
                for (int k = 0; k < NF; ++k) r[k] -= lr * (g * Z[t][k] + 1e-4 * r[k]);
            }
        }

    // --- test sobre texto NUNCA visto ---
    std::vector<std::vector<float>> ring(LH, std::vector<float>(Dd, 0.0f));
    for (int h = 0; h < LH; ++h) {
        const int s = std::max(0, NTR - 1 - h);
        ring[h] = SS[s];
    }
    double bits = 0, bitsBigram = 0;
    int n = 0, cor = 0;

    // bigrama de referencia, estimado SOLO con el train
    std::map<std::pair<int, int>, int> bi;
    std::map<int, int> uni;
    for (int t = 0; t + 1 < NTR; ++t) { bi[{train[t], train[t + 1]}]++; uni[train[t]]++; }

    e.reset_state();
    for (size_t t = 0; t + std::max(1, LAG) < test.size(); ++t) {
        e.feed(test[t]);
        e.theta_features(f);
        for (int h = LH - 1; h > 0; --h) ring[h] = ring[h - 1];
        ring[0] = f;

        const int y = test[t + LAG];
        {
            auto it = bi.find({test[t], test[t + 1]});
            const double c = it == bi.end() ? 0.0 : it->second;
            bitsBigram += -std::log2((c + 0.1) / (uni[test[t]] + 0.1 * 256));
        }
        std::vector<double> z(NF);
        for (int h = 0; h < LH; ++h)
            for (int k = 0; k < K; ++k) z[h * K + k] = (ring[h][sel[k]] - mu[k]) / sd[k];

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

    std::printf("PLASTIC=%d eta=%.1e w_scale=%.2f K=%d LH=%d LAG=%d | train=%d\n", PL, ETA, p.w_scale, K, LH, LAG, NTR);
    std::printf("  TEST (texto no visto, %d simbolos): %.3f bpc | acierto %.1f %%\n", n, bits / n, 100.0 * cor / n);
    std::printf("  bigrama de referencia            : %.3f bpc\n", bitsBigram / n);
    return 0;
}
