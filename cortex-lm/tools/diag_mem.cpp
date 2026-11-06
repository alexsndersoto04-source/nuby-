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
    // --- MODO PROPIO: el usuario mete sus propios numeros ---
    // diag_mem CORPUS N SEED n_fast tau_m_fast tau_m_slow slow_in [tau_m]
    // Si hay 4+ argumentos adicionales, se mide SOLO esa configuracion + BASE
    // y se sale. Si no, se ejecuta el barrido fijo de abajo.
    if (argc > 7) {
        Params p = base;
        p.n_fast      = std::atoi(argv[4]);
        p.tau_m_fast  = (float)std::atof(argv[5]);
        p.tau_m_slow  = (float)std::atof(argv[6]);
        p.slow_in_gain = (float)std::atof(argv[7]);
        if (argc > 8) p.tau_m = (float)std::atof(argv[8]);
        char nombre[64];
        std::snprintf(nombre, sizeof(nombre), "JER %d", p.n_fast);
        mide(p, nombre, txt, SEED);
        return 0;
    }

    { Params p = base; p.ticks_per_char = 24; mide(p, "TICKS 24", txt, SEED); }
    { Params p = base; p.w_scale = 0.6f; mide(p, "RECURR 0.6", txt, SEED); }
    { Params p = base; p.inh_gain = 0.005f; mide(p, "INH 0.005", txt, SEED); }
    // Barrido de memoria que NO depende de la recurrencia: la fuga de membrana.
    // Si el problema es que el estado se degrada en 8 ticks (exp(-8/10)=0,45),
    // subir tau_m conserva mas del presente y del pasado SIN necesidad de
    // recurrencia fuerte (que es la que satura).
    { Params p = base; p.tau_m = 15.0f; mide(p, "TAU 15", txt, SEED); }
    { Params p = base; p.tau_m = 20.0f; mide(p, "TAU 20", txt, SEED); }
    { Params p = base; p.tau_m = 30.0f; mide(p, "TAU 30", txt, SEED); }
    // Fatiga: es la huella del pasado. Relajacion lenta = huella que sobrevive.
    { Params p = base; p.theta_decay = 0.98f; mide(p, "THETA .98", txt, SEED); }
    { Params p = base; p.theta_decay = 0.90f; mide(p, "THETA .90", txt, SEED); }
    // Jerarquia: la poblacion lenta (tau_m_slow=80) deberia retener el pasado.
    { Params p = base; p.n_fast = 512; p.tau_m_fast = 8.0f; p.tau_m_slow = 80.0f;
      p.slow_in_gain = 0.45f; mide(p, "JERARQ", txt, SEED); }
    // Jerarquia con poblacion lenta mas pequena (mas separacion).
    { Params p = base; p.n_fast = 768; p.tau_m_fast = 8.0f; p.tau_m_slow = 120.0f;
      p.slow_in_gain = 0.6f; mide(p, "JER 768", txt, SEED); }
    // Jerarquia extrema: solo 256 neuronas rapidas, 768 lentas.
    { Params p = base; p.n_fast = 256; p.tau_m_fast = 6.0f; p.tau_m_slow = 200.0f;
      p.slow_in_gain = 0.8f; mide(p, "JER 256", txt, SEED); }
    // Configs intermedios (entre JER 768 y JER 256) para mapear el trade-off
    // memoria vs texto. Pequeño n_fast + tau_m_slow grande = mas memoria.
    { Params p = base; p.n_fast = 384; p.tau_m_fast = 6.5f; p.tau_m_slow = 180.0f;
      p.slow_in_gain = 0.75f; mide(p, "JER 384", txt, SEED); }
    { Params p = base; p.n_fast = 320; p.tau_m_fast = 6.0f; p.tau_m_slow = 190.0f;
      p.slow_in_gain = 0.78f; mide(p, "JER 320", txt, SEED); }
    { Params p = base; p.n_fast = 288; p.tau_m_fast = 6.0f; p.tau_m_slow = 200.0f;
      p.slow_in_gain = 0.80f; mide(p, "JER 288", txt, SEED); }
    // Barrido fino del pico de memoria (n_fast 270-300, tau_m_slow 200, slow_in 0.8).
    { Params p = base; p.n_fast = 300; p.tau_m_fast = 6.0f; p.tau_m_slow = 200.0f;
      p.slow_in_gain = 0.80f; mide(p, "JER 300", txt, SEED); }
    { Params p = base; p.n_fast = 290; p.tau_m_fast = 6.0f; p.tau_m_slow = 200.0f;
      p.slow_in_gain = 0.80f; mide(p, "JER 290", txt, SEED); }
    { Params p = base; p.n_fast = 280; p.tau_m_fast = 6.0f; p.tau_m_slow = 200.0f;
      p.slow_in_gain = 0.80f; mide(p, "JER 280", txt, SEED); }
    { Params p = base; p.n_fast = 270; p.tau_m_fast = 6.0f; p.tau_m_slow = 200.0f;
      p.slow_in_gain = 0.80f; mide(p, "JER 270", txt, SEED); }
    // Combinacion: tau_m mas alta + jerarquia.
    { Params p = base; p.tau_m = 15.0f; p.n_fast = 512; p.tau_m_fast = 8.0f;
      p.tau_m_slow = 80.0f; p.slow_in_gain = 0.45f; mide(p, "TAU15+JER", txt, SEED); }

    return 0;
}
