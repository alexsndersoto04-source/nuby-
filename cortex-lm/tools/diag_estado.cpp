// diag_estado — que le hace cada pieza al ESTADO de membrana, sin lector.
//
// No entrena nada: solo presenta texto al motor y mide tres cosas por paso:
//   - |v|/v_th medio         : cuanto esta cargada la poblacion
//   - fraccion en umbral     : cuantas neuronas estan pegadas al techo
//   - varianza entre pasos   : cuanta informacion DISTINTA lleva el estado
//                              (si es constante, el lector no puede separar
//                               una letra de la siguiente, por muy grande que sea)
//
// Uso: diag_estado CORPUS [CARACTERES] [SEMILLA]

#include "cortex_lm.hpp"

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
    double sum_abs = 0.0, sum_peg = 0.0;
    int pasos = 0;
    std::vector<double> perfil(p.layer_size, 0.0);   // media por neurona
    for (unsigned char c : txt) {
        e.feed(c);
        e.state_features(f);
        double a = 0.0, peg = 0.0;
        for (int i = 0; i < (int)f.size(); ++i) {
            a += std::fabs((double)f[i]);
            if (f[i] >= 0.999f) peg += 1.0;
            perfil[i] += f[i];
        }
        sum_abs += a / std::max(1, (int)f.size());
        sum_peg += peg / std::max(1, (int)f.size());
        ++pasos;
    }
    for (double &x : perfil) x /= std::max(1, pasos);
    // varianza entre pasos: se recalcula sobre las features por neurona
    double var = 0.0;
    {
        Engine e2(p);
        e2.build_random(seed);
        std::vector<float> g;
        double s2 = 0.0, s1 = 0.0;
        int m = 0;
        for (unsigned char c : txt) {
            e2.feed(c);
            e2.state_features(g);
            for (int i = 0; i < (int)g.size(); ++i) {
                s2 += (double)g[i] * g[i];
                s1 += g[i];
            }
            m += (int)g.size();
        }
        const double mu = s1 / std::max(1, m);
        var = s2 / std::max(1, m) - mu * mu;
    }
    std::printf("%-22s |v|/v_th medio %.3f | en umbral %5.1f%% | varianza estado %.5f\n",
                nombre, sum_abs / std::max(1, pasos),
                100.0 * sum_peg / std::max(1, pasos), var);
    std::fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "uso: diag_estado CORPUS [N] [SEED]\n"); return 1; }
    auto corpus = carga(argv[1]);
    const int N = argc > 2 ? std::atoi(argv[2]) : 400;
    const unsigned SEED = argc > 3 ? (unsigned)std::atoi(argv[3]) : 1234u;
    std::vector<unsigned char> txt(corpus.begin(), corpus.begin() + std::min((size_t)N, corpus.size()));

    Params base;
    base.seed = SEED;
    base.layer_size = 1024;
    base.ticks_per_char = 8;

    mide(base, "BASE", txt, SEED);
    {
        Params p = base; p.n_pace = 4;
        mide(p, "OSCIL libre", txt, SEED);
        Params q = base; q.n_pace = 4; q.pace_lock = 1;
        mide(q, "OSCIL anclado", txt, SEED);
        Params r = base; r.n_pace = 4; r.pace_lock = 1; r.pace_gain = 0.08f;
        mide(r, "OSCIL anclado gain .08", txt, SEED);
        Params s = base; s.n_pace = 4; s.pace_gain = 0.08f;
        mide(s, "OSCIL libre gain .08", txt, SEED);
    }
    {
        Params p = base; p.n_fast = (int)(p.layer_size * 0.70f);
        mide(p, "JERARQ", txt, SEED);
    }
    return 0;
}
