// bench_bio — las tres piezas nuevas del motor, medidas sobre la SALIDA DE TEXTO.
//
// QUE HAY AQUI
// ------------
// Usa el motor de verdad (engine/cortex_lm.cpp), no un banco aparte. Genera
// texto con tres configuraciones y las imprime para que se puedan comparar a
// ojo, que es la unica prueba que no depende de que yo elija el numero:
//
//   A) BASE     : nada nuevo (todo rapido, sin dopamina, sin oscilador)
//   B) JERARQ   : poblacion lenta + via lenta->rapida con retraso axonal
//   C) TODO     : jerarquia + dopamina (diccionario y bigramas) + oscilador
//
// Tambien imprime contadores: cuantas palabras validas produce cada una, cuantas
// inventadas, y la dopamina media. Si la dopamina no cambia el texto, se vera.
//
// Uso:
//   bench_bio CORPUS DICCIONARIO [CARACTERES] [SEMILLA]
//     CORPUS      texto de entrenamiento (.txt en UTF-8)
//     DICCIONARIO una palabra por linea (sirve el mismo corpus; ver abajo)

#include "cortex_lm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <unordered_set>
#include <string>
#include <vector>

using namespace cortex;

// POR DEFECTO estado + fatiga: medido 3,14-3,16 frente a 3,74 con solo fatiga.
// Con el estado delante, la dopamina ya no ayuda (3,30): ver
// ACTA-ESTADO-DEL-MOTOR.md.
static int g_feat = 2;   // 0 = fatiga, 1 = estado, 2 = estado + fatiga

static std::vector<unsigned char> carga(const char *p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { std::fprintf(stderr, "no se puede abrir %s\n", p); std::exit(1); }
    std::vector<unsigned char> v;
    char c;
    while (in.get(c)) v.push_back((unsigned char)c);
    return v;
}

// El lector local: una capa, softmax sobre las clases del corpus, cada salida
// con SU error. Es el mejor lector medido hasta ahora, y no usa gradientes
// globales (cada fila solo ve su propio error).
struct Lector {
    int F = 0, C = 0;
    std::vector<double> W, b, lg, pr, z;
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

    // `mod` es el TERCER FACTOR: un escalar global (la dopamina) que decide
    // cuanto se consolida este paso. La regla sigue siendo local: cada fila usa
    // solo su error y su entrada; lo unico global es CUANTO aprende.
    double paso(int y, bool aprender, double mod = 1.0) {
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
            if (mod == 0.0) return -std::log2(p);
            for (int c = 0; c < C; ++c) {
                const double g = (pr[c] - (c == y ? 1.0 : 0.0)) * mod;
                if (g == 0.0) continue;
                double *w = &W[(size_t)c * F];
                for (int k = 0; k < F; ++k) w[k] -= tasa * g * z[k];
                b[c] -= tasa * g;
            }
        }
        return -std::log2(p);
    }
};

struct Config {
    const char *nombre;
    bool jerarquia;
    bool dopamina;
    bool oscilador;
    bool refuerzo = false;   // juzgar lo que el propio sistema escribe
    int  epocas = 3;
};

// Genera texto. Devuelve el texto, los bits/caracter y las palabras producidas.
static std::string genera(const Params &base, const Config &cfg,
                          const std::vector<unsigned char> &train,
                          const std::vector<int> &clases, int ngen,
                          unsigned seed, double *bits_out,
                          long *pal_validas, long *pal_ruido,
                          const std::unordered_set<std::string> &dic) {
    Params p = base;
    p.n_fast      = cfg.jerarquia ? (int)(p.layer_size * 0.70f) : 0;
    p.dop_gain    = cfg.dopamina ? 1.0f : 0.0f;
    p.n_pace      = cfg.oscilador ? 4 : 0;

    Engine e(p);
    e.build_random(seed);
    if (cfg.dopamina) {
        // el diccionario ya viene en p.diccionario (lo carga main y lo copia
        // base); aqui solo hace falta que el motor tenga la tabla hash.
    }

    Lector lec;

    std::vector<float> f;
    const int EP = cfg.epocas;
    // dimension de las features segun lo que se le de al lector
    const int FD = (g_feat == 0) ? p.layer_size
                 : (g_feat == 1) ? p.layer_size : 2 * p.layer_size;
    lec.init(FD, clases, 0.05);
    double bits = 0.0;
    int m = 0;
    // ------------------------------------------------------------------
    // BUCLE DE REFUERZO
    // ------------------------------------------------------------------
    // El problema de la primera version: durante el entrenamiento TODO lo que
    // lee es texto real, asi que el juez dice "bien" casi siempre (7.359
    // refuerzos frente a 108 castigos). Una senal que siempre vale lo mismo no
    // informa de nada: la dopamina era una constante disfrazada.
    //
    // Lo que se hace ahora, si el refuerzo esta activado: al final de cada
    // pasada, el sistema ESCRIBE un bloque con lo aprendido, y ese texto
    // inventado se le vuelve a presentar para que el juez lo juzgue. Ahi si hay
    // castigo: sus propias palabras inventadas no estan en el diccionario. Es
    // el ciclo recompensa -> aprende -> escribe -> se juzga -> recompensa.
    std::mt19937 rng_ref(seed * 13u + 5u);
    std::uniform_real_distribution<double> ud_ref(0.0, 1.0);
    for (int ep = 0; ep < EP; ++ep) {
        e.reset_state();
        for (size_t t = 0; t + 1 < train.size(); ++t) {
            e.feed(train[t]);
            if (g_feat == 1) e.state_features(f);
            else if (g_feat == 2) e.full_features(f);
            else e.theta_features(f);
            for (int i = 0; i < (int)f.size(); ++i) lec.z[i] = f[i];
            // el lector tambien recibe el tercer factor: aprende mas cuando la
            // dopamina esta alta y se frena (o se invierte) cuando esta baja.
            const double mod = cfg.dopamina ? (1.0 + e.dopamina()) : 1.0;
            bits += lec.paso(lec.pos[train[t + 1]], true, mod);
            ++m;
        }

        if (cfg.dopamina && cfg.refuerzo) {
            // --- el sistema escribe, y lo que escribe se juzga ---
            const int NB = 120;
            int ultimo = train.back();
            for (int n = 0; n < NB; ++n) {
                e.feed(ultimo);
                if (g_feat == 1) e.state_features(f);
                else if (g_feat == 2) e.full_features(f);
                else e.theta_features(f);
                for (int i = 0; i < (int)f.size(); ++i) lec.z[i] = f[i];
                lec.paso(0, false);
                double suma = 0.0;
                for (int c = 0; c < lec.C; ++c) suma += lec.pr[c];
                double r = ud_ref(rng_ref) * suma, acc = 0.0;
                int elegido = lec.C - 1;
                for (int c = 0; c < lec.C; ++c) {
                    acc += lec.pr[c];
                    if (r <= acc) { elegido = c; break; }
                }
                ultimo = lec.voc[elegido];
                // AHORA se aprende de verdad: la dopamina viene de juzgar lo
                // que el propio sistema acaba de inventar.
                const double mod = 1.0 + e.dopamina();
                if (e.dopamina() != 0.0) lec.paso(elegido, true, mod);
            }
        }
        std::printf("    [%s] pasada %d: %.2f bits/caracter (train)\n",
                    cfg.nombre, ep + 1, bits / std::max(1, m));
        std::fflush(stdout);
    }
    *bits_out = bits / std::max(1, m);
    // cuantas veces el juez emitio refuerzo o castigo durante el entrenamiento
    if (cfg.dopamina)
        std::printf("    [%s] dopamina: %ld refuerzos, %ld castigos, nivel %.2f\n",
                    cfg.nombre, e.dopamina_positivos(), e.dopamina_negativos(),
                    e.dopamina());

    // ---- generacion ----
    std::mt19937 rng(seed * 7u + 3u);
    std::uniform_real_distribution<double> ud(0.0, 1.0);
    e.reset_state();
    const size_t arranque = 40;
    for (size_t t = train.size() - arranque; t + 1 < train.size(); ++t)
        e.feed(train[t]);

    std::string salida;
    int ultimo = train.back();
    long validas = 0, ruido = 0;
    std::string palabra;
    for (int n = 0; n < ngen; ++n) {
        e.feed(ultimo);
        if (g_feat == 1) e.state_features(f);
        else if (g_feat == 2) e.full_features(f);
        else e.theta_features(f);
        for (int i = 0; i < (int)f.size(); ++i) lec.z[i] = f[i];
        lec.paso(0, false);
        // muestreo con temperatura 1
        double suma = 0.0;
        for (int c = 0; c < lec.C; ++c) suma += lec.pr[c];
        double r = ud(rng) * suma, acc = 0.0;
        int elegido = lec.C - 1;
        for (int c = 0; c < lec.C; ++c) {
            acc += lec.pr[c];
            if (r <= acc) { elegido = c; break; }
        }
        ultimo = lec.voc[elegido];
        salida.push_back((char)ultimo);
    }
    (void)palabra;
    (void)validas;
    (void)ruido;
    // metrica sobre el texto generado: palabras reales vs inventadas
    {
        long val = 0, tot = 0;
        std::string w;
        auto cierra = [&]() {
            if (w.size() >= 3) { ++tot; if (dic.count(w)) ++val; }
            w.clear();
        };
        for (unsigned char c : salida) {
            const bool letra = !std::isspace(c) && c != '.' && c != ',' && c != ';'
                            && c != ':' && c != '!' && c != '?' && c != '"'
                            && c != '(' && c != ')' && c < 128;
            if (letra) w.push_back((char)std::tolower(c));
            else cierra();
        }
        cierra();
        *pal_validas = val;
        *pal_ruido = tot;
    }
    return salida;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "uso: bench_bio CORPUS DICCIONARIO [CARACTERES] [SEMILLA]\n");
        return 1;
    }
    const int NGEN = argc > 3 ? std::atoi(argv[3]) : 300;
    const unsigned SEED = argc > 4 ? (unsigned)std::atoi(argv[4]) : 1234u;
    // argv[5]: "JER,DOP,OSC,EPOCAS" para medir cada pieza por separado.
    int usar_jer = 1, usar_dop = 1, usar_osc = 1, EP = 3, mostrar = 1;
    if (argc > 5) {
        std::sscanf(argv[5], "%d,%d,%d,%d", &usar_jer, &usar_dop, &usar_osc, &EP);
    }
    g_feat = argc > 7 ? std::atoi(argv[7]) : 2;   // 0 fatiga, 1 estado, 2 ambas
    if (argc > 6) mostrar = std::atoi(argv[6]);

    auto corpus = carga(argv[1]);
    std::printf("corpus: %zu caracteres\n", corpus.size());

    // vocabulario
    std::vector<int> visto(256, 0);
    for (unsigned char c : corpus) visto[c] = 1;
    std::vector<int> clases;
    for (int c = 0; c < 256; ++c) if (visto[c]) clases.push_back(c);

        Params base;
    base.seed = SEED;
    base.layer_size = 1024;
    base.ticks_per_char = 8;

    // diccionario: el que venga. El motor lo necesita en Params porque
    // build_random() construye la tabla hash.
    {
        std::ifstream in(argv[2]);
        if (!in) {
            std::fprintf(stderr, "no se puede abrir el diccionario %s\n", argv[2]);
            return 1;
        }
        std::string w;
        while (std::getline(in, w)) {
            while (!w.empty() && (w.back() == '\r' || w.back() == ' ')) w.pop_back();
            if (w.size() < 2) continue;
            for (auto &c : w) c = (char)std::tolower((unsigned char)c);
            base.diccionario.push_back(w);
        }
        std::printf("diccionario: %zu palabras\n", base.diccionario.size());
    }

    std::unordered_set<std::string> dic(base.diccionario.begin(),
                                        base.diccionario.end());
    std::vector<Config> cfgs;
    auto C = [&](const char *n, bool j, bool d, bool o, bool r = false) {
        Config c{n, j, d, o, r, EP};
        return c;
    };
    cfgs.push_back(C("BASE   (nada nuevo)", false, false, false));
    if (usar_jer) cfgs.push_back(C("JERARQ (memoria lenta)", true, false, false));
    if (usar_osc) cfgs.push_back(C("OSCIL  (marcapasos)", false, false, true));
    if (usar_dop) cfgs.push_back(C("DOPA   (tercer factor)", false, true, false));
    if (usar_jer && usar_osc) cfgs.push_back(C("JER+OSC", true, false, true));
    if (usar_jer && usar_dop) cfgs.push_back(C("JER+DOPA", true, true, false));
    if (usar_jer && usar_dop && usar_osc) cfgs.push_back(C("TODO", true, true, true));
    // refuerzo sobre lo que el propio sistema escribe
    cfgs.push_back(C("DOPA+REFUERZO", false, true, false, true));

    for (const auto &cfg : cfgs) {
        std::printf("\n===== %s =====\n", cfg.nombre);
        double bits = 0.0;
        long pv = 0, pr = 0;
        std::string txt = genera(base, cfg, corpus, clases, NGEN, SEED,
                                 &bits, &pv, &pr, dic);
        std::printf("  bits/caracter (train): %.2f | palabras reales: %ld de %ld "
                    "(%.1f%%)\n", bits, pv, pr, pr ? 100.0 * pv / pr : 0.0);
        if (mostrar) std::printf("  --- TEXTO ---\n%s\n  -------------\n", txt.c_str());
        std::fflush(stdout);
    }
    return 0;
}
