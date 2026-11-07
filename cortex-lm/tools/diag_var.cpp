// diag_var — separa DOS cosas que la varianza global mezcla.
//
//   var_neurona : cuanto difieren las neuronas ENTRE SI (patron fijo de quien
//                 responde a cada letra). Si solo existe esto, el lector ve una
//                 FOTO del alfabeto: puede decir "esto es una a" pero no "viene
//                 despues de una q".
//   var_tiempo  : cuanto cambia CADA neurona a lo largo del texto. Esto es lo
//                 unico que codifica el contexto; sin ello no hay memoria.
//
// Se mide tambien la correlacion entre el estado del caracter t y el del t-1
// (cuanto dura la huella) y cuantas neuronas son "muertas" (nunca se mueven).
//
// Uso: diag_var CORPUS [N] [SEED]

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

static void mide(Params p, const char *nombre,
                 const std::vector<unsigned char> &txt, unsigned seed) {
    Engine e(p);
    e.build_random(seed);
    std::vector<float> f;
    const int N = p.layer_size;
    std::vector<std::vector<float>> pasos;     // estado por caracter
    for (unsigned char c : txt) {
        e.feed(c);
        e.state_features(f);
        pasos.emplace_back(f.begin(), f.begin() + std::min((int)f.size(), N));
    }
    const int T = (int)pasos.size();
    if (T < 3) return;

    // medias por neurona y por caracter
    std::vector<double> mu_n(N, 0.0), v_t(N, 0.0), mu_t(T, 0.0);
    double total = 0.0;
    for (int t = 0; t < T; ++t) {
        double s = 0.0;
        for (int j = 0; j < N; ++j) { mu_n[j] += pasos[t][j]; s += pasos[t][j]; }
        mu_t[t] = s / N;
        total += s / N;
    }
    for (int j = 0; j < N; ++j) mu_n[j] /= T;
    total /= T;                      // <-- sin esto la comparacion es basura
                                     //     (el "total" era la SUMA de T medias)
    for (int t = 0; t < T; ++t)
        for (int j = 0; j < N; ++j) {
            const double d = pasos[t][j] - mu_n[j];
            v_t[j] += d * d;
        }
    for (int j = 0; j < N; ++j) v_t[j] /= T;

    // var entre neuronas = varianza de las medias temporales
    double var_neurona = 0.0;
    for (int j = 0; j < N; ++j) {
        const double d = mu_n[j] - total;
        var_neurona += d * d;
    }
    var_neurona /= N;
    // var temporal = media de las varianzas temporales de cada neurona
    double var_tiempo = 0.0;
    int muertas = 0;
    for (int j = 0; j < N; ++j) {
        var_tiempo += v_t[j];
        if (std::fabs(mu_n[j]) < 1e-4 && v_t[j] < 1e-8) ++muertas;
    }
    var_tiempo /= N;

    // correlacion estado(t) con estado(t-1): cuanto sobrevive la huella
    auto corr = [&](int lag) {
        double a = 0.0, b = 0.0, ab = 0.0;
        for (int t = lag; t < T; ++t)
            for (int j = 0; j < N; ++j) {
                const double x = pasos[t][j] - mu_n[j], y = pasos[t - lag][j] - mu_n[j];
                a += x * x; b += y * y; ab += x * y;
            }
        return (a > 1e-12 && b > 1e-12) ? ab / std::sqrt(a * b) : 0.0;
    };
    std::printf("%-12s var_neurona %.6f | var_tiempo %.6f | ratio t/n %6.2f | "
                "corr(t,t-1) %+.3f | corr(t,t-4) %+.3f | muertas %3d/%d\n",
                nombre, var_neurona, var_tiempo, var_tiempo / std::max(1e-12, var_neurona),
                corr(1), corr(4), muertas, N);
    std::fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "uso: diag_var CORPUS [N] [SEED]\n"); return 1; }
    auto corpus = carga(argv[1]);
    const int N = argc > 2 ? std::atoi(argv[2]) : 400;
    const unsigned SEED = argc > 3 ? (unsigned)std::atoi(argv[3]) : 1234u;
    std::vector<unsigned char> txt(corpus.begin(),
                                   corpus.begin() + std::min((size_t)N, corpus.size()));
    Params base;
    base.seed = SEED;
    base.layer_size = 1024;
    base.ticks_per_char = 8;
    mide(base, "BASE", txt, SEED);
    { Params p = base; p.n_fast = (int)(p.layer_size * 0.70f); mide(p, "JERARQ", txt, SEED); }
    { Params p = base; p.n_pace = 4; mide(p, "OSCIL", txt, SEED); }
    { Params p = base; p.w_scale = 0.0f; mide(p, "SIN RECURR.", txt, SEED); }
    { Params p = base; p.ticks_per_char = 24; mide(p, "TICKS 24", txt, SEED); }
    return 0;
}
