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
// Ganancia del marcapasos a probar (0 = no probar). Ver tools/diag_estado.cpp:
// a 0,35 el oscilador mete 5x mas corriente que el propio texto.
static float g_pace_gain = 0.0f;
// Cuantos estados pasados ve el lector (0 = solo el actual). Ver
// Engine::tap_features: el estado no se acuerda del caracter anterior, asi que
// con una sola foto el techo es el bigrama.
static int g_taps = 1;
// Parametros del reservorio que se pueden mover por CLI (0 = dejar el default).
static float g_w_scale = 0.0f;
static float g_inh = 0.0f;
// Normalizacion del lector: 1 = por dimension (compara configuraciones del
// reservorio en igualdad de condiciones), 0 = como siempre (serie historica).
static int g_norm = 1;   // normalizacion calibrada: ESSENCIAL. Sin ella el lector
                         // no aprende (features en bruto -> entropia de unigrama,
                         // 4,4+ bits). La normalizacion se calibra sobre 1500
                         // chars, se congela y se usa para todos los configs.
static int g_hidden = 0;     // 0 = lector lineal, >0 = expansión aleatoria
static float g_fscale = 1.0f; // escala de las features (1.0 = sin escalar)
static int g_ncal = 1500;      // caracteres con que se calibra la normalizacion
static int g_tpc = 8;          // ticks por simbolo (palanca de memoria mas fuerte)
// Jerarquia: cuántas neuronas son rápidas (0 = no jerarquia).
static int g_nfast = 0;
static float g_tf = 0.0f, g_ts = 0.0f, g_sig = 0.0f;

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
    int F = 0, C = 0, H = 0;
    std::vector<double> W, b, lg, pr, z;
    std::vector<double> R, h;   // R: proyeccion aleatoria fija, h: activacion hidden
    std::vector<int> voc, pos;
    double tasa = 0.05;
    unsigned seed_r = 12345;
    // NORMALIZACION POR DIMENSION, CALIBRADA UNA VEZ Y CONGELADA.
    // Sin esto el lector no puede comparar dos configuraciones del reservorio
    // (con INH 0,005 la membrana se pega al techo y salen 6,54 bits con 70
    // clases, imposible). Normalizar EN LINEA tampoco vale: amplifica el ruido
    // de las dimensiones casi muertas y se midieron 24,9 bits. Se calibra sobre
    // un trozo de texto, se congela, y las dimensiones con desviacion
    // despreciable se dejan a cero en vez de amplificarlas.
    std::vector<double> mu, sd, escala;
    bool calibrado = false;   // solo si g_norm

    void init(int nf, const std::vector<int> &clases, double t, int hidden, unsigned seed) {
        F = nf; C = (int)clases.size(); H = hidden; voc = clases; tasa = t;
        seed_r = seed;
        pos.assign(256, -1);
        for (int i = 0; i < C; ++i) pos[voc[i]] = i;
        if (H > 0) {
            R.assign((size_t)H * F, 0.0);
            h.assign(H, 0.0);
            std::mt19937 gen(seed_r);
            std::uniform_real_distribution<double> dist(-1.0, 1.0);
            for (auto &v : R) v = dist(gen) / std::sqrt((double)F);
            W.assign((size_t)C * H, 0.0);
        } else {
            W.assign((size_t)C * F, 0.0);
        }
        b.assign(C, 0.0);
        lg.assign(C, 0.0);
        pr.assign(C, 0.0);
        z.assign(F, 0.0);
        mu.assign(F, 0.0);
        sd.assign(F, 0.0);
        escala.assign(F, 1.0);
    }

    // paso de calibracion: acumula media y desviacion, no aprende
    void calibra(const std::vector<float> &f, double n) {
        for (int k = 0; k < F && k < (int)f.size(); ++k) {
            const double d = f[k] - mu[k];
            mu[k] += d / n;
            sd[k] += d * (f[k] - mu[k]);
        }
    }

    void congela(double n) {
        double mx = 0.0;
        std::vector<double> s(F, 0.0);
        for (int k = 0; k < F; ++k) {
            s[k] = std::sqrt(std::max(0.0, sd[k]) / std::max(1.0, n));
            mx = std::max(mx, s[k]);
        }
        const double piso = 0.05 * mx;        // por debajo, la dimension es ruido
        for (int k = 0; k < F; ++k) escala[k] = s[k] >= piso && s[k] > 1e-9 ? 1.0 / s[k] : 0.0;
        calibrado = true;
    }

    void fija(const std::vector<float> &f) {
        // /sqrt(F): con la normalizacion la entrada deja de ser diminuta (antes
        // z ~ 0,06), asi que sin este factor la tasa 0,05 queda miles de veces
        // demasiado grande y los logits se van a 24 bits/caracter. Dividir por
        // sqrt(F) devuelve el producto interno a la misma escala de antes y la
        // tasa sigue valiendo.
        const bool act = calibrado && g_norm != 0;
        const double inv = act ? 1.0 / std::sqrt((double)std::max(1, F)) : 1.0;
        for (int k = 0; k < F; ++k) {
            const float x = k < (int)f.size() ? f[k] : 0.0f;
            z[k] = act ? inv * escala[k] * (x - mu[k]) : (double)x * g_fscale;
        }
    }

    // `mod` es el TERCER FACTOR: un escalar global (la dopamina) que decide
    // cuanto se consolida este paso. La regla sigue siendo local: cada fila usa
    // solo su error y su entrada; lo unico global es CUANTO aprende.
    double paso(int y, bool aprender, double mod = 1.0) {
        // Calcular h = tanh(R * z) si hay hidden layer, o h = z si no.
        if (H > 0) {
            for (int k = 0; k < H; ++k) {
                double acc = 0.0;
                const double *rk = &R[(size_t)k * F];
                for (int j = 0; j < F; ++j) acc += rk[j] * z[j];
                h[k] = std::tanh(acc);
            }
        }
        double mx = -1e30, sm = 0.0;
        const int D = H > 0 ? H : F;
        const double * __restrict__ in_ptr = H > 0 ? h.data() : z.data();
        for (int c = 0; c < C; ++c) {
            const double * __restrict__ w = &W[(size_t)c * D];
            double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
            int k = 0;
            for (; k + 3 < D; k += 4) {
                a0 += w[k] * in_ptr[k];
                a1 += w[k + 1] * in_ptr[k + 1];
                a2 += w[k + 2] * in_ptr[k + 2];
                a3 += w[k + 3] * in_ptr[k + 3];
            }
            double a = b[c] + (a0 + a1) + (a2 + a3);
            for (; k < D; ++k) a += w[k] * in_ptr[k];
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
                const double step_g = tasa * g;
                double * __restrict__ w = &W[(size_t)c * D];
                for (int k = 0; k < D; ++k) w[k] -= step_g * in_ptr[k];
                b[c] -= step_g;
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
    bool anclado = false;    // oscilador con fase reiniciada por simbolo
    float pace_gain = 0.35f; // corriente del marcapasos (se mide, no se fija)
};

// Rellena `f` con lo que ve el lector: los `g_taps` ultimos estados (y la
// fatiga si FEAT=2, solo del caracter actual: la fatiga ya lleva su propia cola).
static void features(const Engine &e, std::vector<float> &f, std::vector<float> &aux) {
    if (g_feat == 0) e.theta_features(f);
    else if (g_feat == 1) e.tap_features(f, g_taps);
    else {
        e.tap_features(f, g_taps);
        e.theta_features(aux);
        f.insert(f.end(), aux.begin(), aux.end());
    }
}

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
    p.pace_lock   = (cfg.oscilador && cfg.anclado) ? 1 : 0;
    p.pace_gain   = cfg.pace_gain;
    if (g_w_scale > 0.0f) p.w_scale = g_w_scale;
    if (g_inh > 0.0f)     p.inh_gain = g_inh;
    // Parametros de jerarquia: solo se aplican cuando el config pide jerarquia.
    // BASE (jerarquia=false) debe quedar limpio, sin los valores de CLI.
    if (cfg.jerarquia) {
        if (g_nfast > 0)      p.n_fast = g_nfast;
        if (g_tf > 0.0f)      p.tau_m_fast = g_tf;
        if (g_ts > 0.0f)      p.tau_m_slow = g_ts;
        if (g_sig > 0.0f)     p.slow_in_gain = g_sig;
    }

    Engine e(p);
    e.build_random(seed);
    if (cfg.dopamina) {
        // el diccionario ya viene en p.diccionario (lo carga main y lo copia
        // base); aqui solo hace falta que el motor tenga la tabla hash.
    }

    Lector lec;

    std::vector<float> f, aux;
    e.tap_hist_reserve(std::max(1, g_taps));
    const int EP = cfg.epocas;
    // dimension de las features segun lo que se le de al lector.
    // features() produces: g_feat==0 -> theta (N); g_feat==1 -> g_taps*N;
    // g_feat==2 -> g_taps*N + theta (N). El FD tiene que coincidir con eso,
    // o el lector recibe un vector de otra longitud y las dimensiones extra
    // se caen (con g_taps=1 daba 2N y coincidía; con g_taps>=2 estaba roto).
    const int Nlayer = p.layer_size;
    const int FD = (g_feat == 0) ? Nlayer
                 : (g_feat == 1) ? Nlayer * std::max(1, g_taps)
                                 : Nlayer * std::max(1, g_taps) + Nlayer;
    lec.init(FD, clases, 0.05, g_hidden, seed);
    // --- calibracion de la normalizacion ---
    // Solo cuando g_norm esta activado. Sin normalizacion, no hay calibracion
    // y el lector recibe las features tal cual.
    if (g_norm) {
        const int NCAL = std::min<int>(g_ncal, (int)train.size());
        for (int i = 0; i < NCAL; ++i) {
            e.feed(train[i]);
            features(e, f, aux);
            lec.calibra(f, i + 1.0);
        }
        lec.congela((double)NCAL);
        e.reset_state();
    }
    double bits = 0.0;
    int m = 0;
    // --- particion secuencial: 80% entreno, 20% holdout (Nunca mezcladas) ---
    const size_t NTR = train.size() * 0.8;
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
        for (size_t t = 0; t + 1 < NTR; ++t) {
            e.feed(train[t]);
            features(e, f, aux);
            lec.fija(f);
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
                features(e, f, aux);
                lec.fija(f);
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
    // --- holdout: tramo NO VISTO (secuencial), sin aprender ---
    double hbits = 0.0;
    int hm = 0;
    e.reset_state();
    for (size_t t = NTR; t + 1 < train.size(); ++t) {
        e.feed(train[t]);
        features(e, f, aux);
        lec.fija(f);
        hbits += lec.paso(lec.pos[train[t + 1]], false);
        ++hm;
    }
    std::printf("    [%s] bits/caracter (holdout): %.2f (%d chars)\n",
                cfg.nombre, hbits / std::max(1, hm), (int)(train.size() - NTR));
    std::fflush(stdout);
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
        features(e, f, aux);
        lec.fija(f);
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
                     "uso: bench_bio CORPUS DICC [NGEN] [SEED] [\"JER,DOP,OSC,EP\"] [mostrar] [FEAT] [PACE_GAIN]\n");
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
    if (argc > 8) g_pace_gain = (float)std::atof(argv[8]);   // ganancia del marcapasos
    g_taps = argc > 9 ? std::max(1, std::atoi(argv[9])) : 1;  // estados pasados
    if (argc > 10) g_w_scale = (float)std::atof(argv[10]);    // recurrencia
    if (argc > 11) g_inh = (float)std::atof(argv[11]);        // inhibicion
    if (argc > 12) g_norm = std::atoi(argv[12]);               // 1 = normalizar lector
    if (argc > 13) g_nfast = std::atoi(argv[13]);               // n_fast (jerarquia)
    if (argc > 14) g_tf = (float)std::atof(argv[14]);             // tau_m_fast
    if (argc > 15) g_ts = (float)std::atof(argv[15]);             // tau_m_slow
    if (argc > 16) g_sig = (float)std::atof(argv[16]);            // slow_in_gain
    if (argc > 17) g_hidden = std::atoi(argv[17]);                // hidden layer size
    if (argc > 18) g_fscale = (float)std::atof(argv[18]);           // feature scale
    // Cantidad de caracteres con los que se CALIBRA la normalizacion del
    // lector. Estuvo fijo a 1500; con g_taps>=2 el lector tiene 3*N
    // dimensiones y 1500 muestras es poco, así que se puede mover.
    if (argc > 19) g_ncal = std::atoi(argv[19]);                     // calibracion
    if (argc > 20) g_tpc = std::atoi(argv[20]);                      // ticks/simbolo

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
    base.ticks_per_char = g_tpc > 0 ? g_tpc : 8;

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
        Config c{n, j, d, o, r, EP, false, 0.35f};
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
    if (usar_dop) cfgs.push_back(C("DOPA+REFUERZO", false, true, false, true));
    // oscilador ANCLADO al simbolo (fase reiniciada cada 8 ticks)
    if (usar_osc) {
        Config ca{"OSCIL+ANCLADO", false, false, true, false, EP, true, 0.35f};
        cfgs.push_back(ca);
        if (g_pace_gain > 0.0f) {
            Config cg{"OSCIL+ANCLADO gain", false, false, true, false, EP, true, g_pace_gain};
            cfgs.push_back(cg);
        }
        if (usar_jer) {
            Config cj{"JER+OSC+ANCLADO", true, false, true, false, EP, true, 0.35f};
            cfgs.push_back(cj);
        }
    }

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
