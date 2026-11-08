// bench_cortical.cpp — Verificacion multi-semilla, memoria multi-lag y escala CSR
// de la jerarquia cortical LIF pura (100% neuronas spiking LIF + reglas locales,
// cero backpropagation, memoria O(1) constante respecto a la longitud del texto).
//
// Modos:
//   bench_cortical mem   <L1> <L2> <seed>
//   bench_cortical bpc   <corpus> <dic> <L1> <L2> <epocas> <lr> <stdp> <seed>
//   bench_cortical scale <capas_csv> <max_deg> <chars_prueba>

#include "cortex_lm.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace cortex;

static size_t current_rss_kb() {
    std::ifstream f("/proc/self/statm");
    size_t pages = 0, rss = 0;
    if (f >> pages >> rss) return rss * 4;
    return 0;
}

static std::string cargar_archivo(const char* ruta) {
    std::ifstream f(ruta, std::ios::binary);
    if (!f) { std::fprintf(stderr, "No pude abrir: %s\n", ruta); std::exit(1); }
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

static std::unordered_set<std::string> cargar_dic(const char* ruta) {
    std::unordered_set<std::string> d;
    std::ifstream f(ruta);
    std::string w;
    while (f >> w) d.insert(w);
    return d;
}

static std::string minuscula_utf8(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        uint8_t c = (uint8_t)s[i];
        if (c >= 'A' && c <= 'Z') r.push_back((char)(c + 32));
        else if (c == 0xC3 && i + 1 < s.size()) {
            uint8_t d = (uint8_t)s[i + 1];
            if (d >= 0x80 && d <= 0x9E) d += 0x20;
            r.push_back((char)c);
            r.push_back((char)d);
            ++i;
        } else r.push_back((char)c);
    }
    return r;
}

static bool es_letra_es(const std::string& s, size_t i, size_t& adv) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) { adv = 1; return true; }
    if (c == 0xC3 && i + 1 < s.size()) {
        uint8_t d = (uint8_t)s[i + 1];
        if ((d >= 0x80 && d <= 0x9E) || (d >= 0xA0 && d <= 0xBE)) { adv = 2; return true; }
    }
    adv = 1;
    return false;
}

static std::pair<int, int> contar_palabras_reales(
    const std::string& texto, const std::unordered_set<std::string>& dic) {
    int tot = 0, ok = 0;
    size_t i = 0;
    while (i < texto.size()) {
        size_t adv = 1;
        if (!es_letra_es(texto, i, adv)) { i += adv; continue; }
        size_t j = i;
        int n_letras = 0;
        while (j < texto.size()) {
            size_t a2 = 1;
            if (!es_letra_es(texto, j, a2)) break;
            j += a2;
            n_letras++;
        }
        if (n_letras >= 3) {
            tot++;
            if (dic.count(minuscula_utf8(texto.substr(i, j - i)))) ok++;
        }
        i = j;
    }
    return {ok, tot};
}

static bool utf8_transicion_valida(uint8_t prev_lead_rem, uint8_t cand) {
    const bool is_cont = (cand >= 0x80 && cand <= 0xBF);
    if (prev_lead_rem > 0) return is_cont;
    if (is_cont) return false;
    return true;
}

static int utf8_rem_after(uint8_t prev_lead_rem, uint8_t b) {
    if (prev_lead_rem > 0) return (int)prev_lead_rem - 1;
    if ((b & 0xE0) == 0xC0) return 1;
    if ((b & 0xF0) == 0xE0) return 2;
    if ((b & 0xF8) == 0xF0) return 3;
    return 0;
}

// ============================================================================
// MODO 1: Sonda de memoria de trabajo multi-lag (lags 0..5)
// Protocolo identico a diag_mem.cpp (26 letras equiprobables sin gramatica,
// 3000 train + 1000 test independientes; azar puro = 3.8%).
// ============================================================================
static std::vector<int> build_bench_layers(int L1, const char* l2_rest) {
    std::vector<int> ls = {1024, L1};
    std::stringstream ss(l2_rest);
    std::string it;
    while (std::getline(ss, it, ',')) {
        if (!it.empty()) ls.push_back(std::stoi(it));
    }
    return ls;
}

static int modo_mem(int L1, const char* l2_rest, uint32_t seed) {
    Params p;
    p.cortical_hierarchy = true;
    p.layer_sizes = build_bench_layers(L1, l2_rest);
    p.max_out_degree = 52;
    p.ticks_per_char = 8;

    Engine eng(p);
    eng.build_random(seed);
    const int F = eng.cortical_feature_dim(2);

    std::mt19937 rng(seed + 77u);
    const int NTRAIN = 3000, NTEST = 1000;
    std::vector<uint8_t> seq(NTRAIN + NTEST);
    for (auto& x : seq) x = (uint8_t)('a' + (rng() % 26));

    const int NLAGS = 6;
    std::vector<CorticalReadout> probes(NLAGS);
    for (int l = 0; l < NLAGS; ++l) {
        probes[(size_t)l].init(F, 0.35f, 0.0f);
        for (int c = 'a'; c <= 'z'; ++c) probes[(size_t)l].ensure_byte_slot((uint8_t)c);
    }

    std::vector<std::pair<i32, float>> feats;
    eng.reset_state();
    eng.set_plasticity(false);
    for (int i = 0; i < 600; ++i) {
        eng.feed(seq[(size_t)i]);
        eng.cortical_sparse_features(feats, 2);
        for (int l = 0; l < NLAGS; ++l) probes[(size_t)l].calibrate_step(feats);
    }
    for (int l = 0; l < NLAGS; ++l) probes[(size_t)l].freeze_calibration();

    for (int ep = 0; ep < 3; ++ep) {
        eng.reset_state();
        for (int l = 0; l < NLAGS; ++l) probes[(size_t)l].set_lr(0.35f * (1.0f - 0.2f * ep));
        for (int i = 0; i < NTRAIN; ++i) {
            eng.feed(seq[(size_t)i]);
            eng.cortical_sparse_features(feats, 2);
            for (int l = 0; l < NLAGS; ++l) {
                if (i >= l) probes[(size_t)l].step(seq[(size_t)(i - l)], feats, true, 1.0f);
            }
        }
    }

    std::vector<int> hits(NLAGS, 0);
    std::vector<double> probs(256);
    for (int i = NTRAIN; i < NTRAIN + NTEST; ++i) {
        eng.feed(seq[(size_t)i]);
        eng.cortical_sparse_features(feats, 2);
        for (int l = 0; l < NLAGS; ++l) {
            probes[(size_t)l].predict(feats, probs.data(), 1.0);
            int best = 'a';
            for (int c = 'a'; c <= 'z'; ++c)
                if (probs[(size_t)c] > probs[(size_t)best]) best = c;
            if ((uint8_t)best == seq[(size_t)(i - l)]) hits[(size_t)l]++;
        }
    }

    std::printf("MEMORIA CORTICAL LIF {1024,%d,%s} (seed=%-4u, azar=3.8%%):", L1, l2_rest, seed);
    for (int l = 0; l < NLAGS; ++l)
        std::printf("  lag%d=%5.1f%%", l, 100.0 * hits[(size_t)l] / NTEST);
    std::printf("\n");
    return 0;
}

// ============================================================================
// MODO 2: Evaluacion BPC cronologica 80/20 + generacion con diccionario
// ============================================================================
static int modo_bpc(const char* ruta_corpus, const char* ruta_dic,
                    int L1, const char* l2_rest, int epocas, float lr0, int stdp_on, uint32_t seed) {
    const std::string data = cargar_archivo(ruta_corpus);
    const auto dic = cargar_dic(ruta_dic);
    const size_t N = data.size();
    const size_t NTR = (size_t)(N * 0.80);

    Params p;
    p.cortical_hierarchy = true;
    p.layer_sizes = build_bench_layers(L1, l2_rest);
    p.max_out_degree = 52;
    p.ticks_per_char = 8;
    if (!stdp_on) {
        p.stdp_ltp = 0.0f;
        p.stdp_ltd = 0.0f;
        p.homeo_thr_up = 0.0f;
        p.homeo_thr_down = 0.0f;
    }

    auto t_start = std::chrono::steady_clock::now();
    Engine eng(p);
    eng.build_random(seed);

    CorticalReadout rd;
    rd.init(eng.cortical_feature_dim(2), lr0, 0.0f);

    std::vector<std::pair<i32, float>> feats;

    // 1a. Periodo critico de desarrollo cortical (STDP no supervisado puro)
    if (stdp_on != 0) {
        eng.reset_state();
        eng.set_plasticity(true);
        const size_t N_CRIT = std::min<size_t>(NTR, 60000);
        for (size_t i = 0; i < N_CRIT; ++i) eng.feed((uint8_t)data[i]);
    }

    // 1b. Calibracion con la corteza estabilizada
    eng.reset_state();
    eng.set_plasticity(false);
    const size_t NCAL = std::min<size_t>(2500, NTR);
    for (size_t i = 0; i < NCAL; ++i) {
        eng.feed((uint8_t)data[i]);
        eng.cortical_sparse_features(feats, 2);
        rd.calibrate_step(feats);
    }
    rd.freeze_calibration();

    // 2. Entrenamiento secuencial sobre el 80% inicial
    double last_tr_bpc = 0.0;
    for (int ep = 0; ep < epocas; ++ep) {
        eng.reset_state();
        eng.set_plasticity(false);
        rd.set_lr(lr0 / (1.0f + 0.05f * ep));

        double bits = 0.0;
        size_t cnt = 0;
        for (size_t t = 0; t + 1 < NTR; ++t) {
            eng.feed((uint8_t)data[t]);
            eng.cortical_sparse_features(feats, 2);
            bits += rd.step((uint8_t)data[t + 1], feats, true, 1.0f);
            cnt++;
        }
        last_tr_bpc = bits / (double)cnt;
    }

    // 3. Evaluar holdout (20% final jamas visto) sin aprender ni STDP
    eng.reset_state();
    eng.set_plasticity(false);
    // Calentar contexto con los ultimos 64 simbolos del train (sin evaluar alli)
    for (size_t t = (NTR > 64 ? NTR - 64 : 0); t < NTR; ++t) eng.feed((uint8_t)data[t]);
    double hbits = 0.0;
    size_t hcnt = 0, act_sum = 0;
    for (size_t t = NTR; t + 1 < N; ++t) {
        eng.feed((uint8_t)data[t]);
        eng.cortical_sparse_features(feats, 2);
        act_sum += feats.size();
        hbits += rd.step((uint8_t)data[t + 1], feats, false, 0.0f);
        hcnt++;
    }
    const double ho_bpc = hbits / (double)hcnt;
    const double avg_act = (double)act_sum / (double)hcnt;

    // 4. Generacion autorregresiva pura LIF (300 bytes, UTF-8 limpio)
    eng.reset_state();
    uint8_t u8_rem = 0;
    for (size_t t = NTR - 60; t < NTR; ++t) {
        uint8_t b = (uint8_t)data[t];
        eng.feed(b);
        u8_rem = (uint8_t)utf8_rem_after(u8_rem, b);
    }
    std::mt19937 grng(seed * 7u + 3u);
    std::vector<double> probs(256);
    std::string gen;
    for (int g = 0; g < 300; ++g) {
        eng.cortical_sparse_features(feats, 2);
        rd.predict(feats, probs.data(), 0.78);
        double sum = 0.0;
        for (int b = 0; b < 256; ++b) {
            if (!utf8_transicion_valida(u8_rem, (uint8_t)b)) probs[(size_t)b] = 0.0;
            sum += probs[(size_t)b];
        }
        uint8_t pick = ' ';
        if (sum > 0.0) {
            std::uniform_real_distribution<double> ud(0.0, sum);
            double r = ud(grng), acc = 0.0;
            for (int b = 0; b < 256; ++b) {
                acc += probs[(size_t)b];
                if (r <= acc) { pick = (uint8_t)b; break; }
            }
        }
        gen.push_back((char)pick);
        u8_rem = (uint8_t)utf8_rem_after(u8_rem, pick);
        eng.feed(pick);
    }
    auto [ok_w, tot_w] = contar_palabras_reales(gen, dic);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();

    std::printf("SEED=%-4u | {1024,%d,%s} (%zu syn) | stdp=%d | train=%.3f | HOLDOUT=%.3f bpc | palabras=%2d/%2d (%5.1f%%) | act=%.0f | %.1fs\n",
                seed, L1, l2_rest, eng.n_synapses(), stdp_on,
                last_tr_bpc, ho_bpc, ok_w, tot_w,
                tot_w ? 100.0 * ok_w / tot_w : 0.0, avg_act, secs);
    std::fflush(stdout);
    return 0;
}

// ============================================================================
// MODO 3: Prueba de escalabilidad CSR y memoria constante O(1) en secuencia
// ============================================================================
static int modo_scale(const char* capas_csv, int max_deg, size_t chars_prueba) {
    Params p;
    p.cortical_hierarchy = true;
    p.layer_sizes.clear();
    {
        std::stringstream ss(capas_csv);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (!item.empty()) p.layer_sizes.push_back(std::stoi(item));
        }
    }
    if (p.layer_sizes.empty()) p.layer_sizes = {1024};
    if (p.layer_sizes.size() == 1) p.cortical_hierarchy = false;
    p.max_out_degree = max_deg;
    p.ticks_per_char = 8;

    const size_t rss_before = current_rss_kb();
    auto t0 = std::chrono::steady_clock::now();
    Engine eng(p);
    eng.build_random(42);
    auto t1 = std::chrono::steady_clock::now();
    const size_t rss_init = current_rss_kb();

    const char* frase = "En un lugar de la Mancha, de cuyo nombre no quiero acordarme, no ha mucho tiempo ";
    const size_t flen = std::strlen(frase);

    const size_t half = chars_prueba / 2;
    auto tf0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < half; ++i) eng.feed((uint8_t)frase[i % flen]);
    const size_t rss_mid = current_rss_kb();
    for (size_t i = half; i < chars_prueba; ++i) eng.feed((uint8_t)frase[i % flen]);
    auto tf1 = std::chrono::steady_clock::now();
    const size_t rss_end = current_rss_kb();

    const double build_s = std::chrono::duration<double>(t1 - t0).count();
    const double feed_s = std::chrono::duration<double>(tf1 - tf0).count();
    const double cps = chars_prueba / std::max(1e-9, feed_s);
    const double net_mb = (double)(rss_init > rss_before ? rss_init - rss_before : rss_init) / 1024.0;
    const long drift_kb = (long)rss_end - (long)rss_mid;

    std::printf("ESCALA %-22s | neuronas=%7d | sinapsis=%10zu | init=%5.2fs | RAM_red=%6.1f MB | vel=%7.0f chars/s | deriva_RSS=%+ld KB\n",
                capas_csv, eng.n_neurons(), eng.n_synapses(),
                build_s, net_mb, cps, drift_kb);
    std::fflush(stdout);
    return 0;
}

static int modo_macro(const char* capas_csv, int macro_cols, int top_k_cols, size_t chars_prueba) {
    Params p;
    p.cortical_hierarchy = true;
    p.layer_sizes.clear();
    {
        std::stringstream ss(capas_csv);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (!item.empty()) p.layer_sizes.push_back(std::stoi(item));
        }
    }
    if (p.layer_sizes.empty()) p.layer_sizes = {2048, 32768, 65536, 262144, 163840};
    p.max_out_degree = 32;
    p.assoc_fan_in_slot = 4;
    p.ticks_per_char = 8;
    p.n_macro_cols = macro_cols;
    p.trn_top_k = top_k_cols;

    const size_t rss_before = current_rss_kb();
    auto t0 = std::chrono::steady_clock::now();
    Engine eng(p);
    eng.build_random(2026);
    CompactCorticalReadout rd_v, rd_d;
    rd_v.init(eng.cortical_feature_dim(2), 0.15f);
    rd_d.init(eng.cortical_feature_dim(2), 0.25f);
    auto t1 = std::chrono::steady_clock::now();

    const char* texto =
        "La corteza cerebral integra columnas talamocorticales y dendritas apicales para razonar "
        "mediante espigas reales en C++ sin retropropagacion ni atajos artificiales. ";
    const size_t tlen = std::strlen(texto);

    std::vector<std::pair<i32, float>> feats;
    const size_t half = chars_prueba / 2;
    auto tf0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < half; ++i) {
        const uint8_t ch = (uint8_t)texto[i % tlen];
        eng.feed(ch);
        if (i >= 16) {
            eng.cortical_sparse_features(feats, 2);
            rd_v.step((uint8_t)texto[(i + 1) % tlen], feats, true, 1.0f);
        }
    }
    const size_t rss_mid = current_rss_kb();
    for (size_t i = half; i < chars_prueba; ++i) {
        const uint8_t ch = (uint8_t)texto[i % tlen];
        eng.feed(ch);
        eng.cortical_sparse_features(feats, 2);
        rd_v.step((uint8_t)texto[(i + 1) % tlen], feats, true, 1.0f);
    }
    auto tf1 = std::chrono::steady_clock::now();
    const size_t rss_end = current_rss_kb();

    const double build_s = std::chrono::duration<double>(t1 - t0).count();
    const double run_s   = std::chrono::duration<double>(tf1 - tf0).count();
    const double cps     = chars_prueba / std::max(1e-9, run_s);
    const double ram_mb  = (double)(rss_end > rss_before ? rss_end - rss_before : rss_end) / 1024.0;
    const double rd_mb   = (double)(rd_v.memory_bytes() + rd_d.memory_bytes()) / (1024.0 * 1024.0);
    const long drift_kb  = (long)rss_end - (long)rss_mid;
    auto cols = eng.active_macro_columns(-1);

    std::printf("MACRO-CORTEZA %s (%d regiones, TRN %d/%d cols, %d hilos CPU)\n"
                "  Neuronas LIF puras : %d\n"
                "  Sinapsis CSR reales: %zu\n"
                "  Filas FP16 activas : %zu (lectores V+D = %.1f MB | RAM total = %.1f MB)\n"
                "  Inicializacion     : %.2f s | Velocidad train+LIF: %.0f chars/s | Deriva RSS: %+ld KB\n"
                "  Columnas TRN ganadoras: ",
                capas_csv, eng.num_layers(), top_k_cols, macro_cols, Engine::max_cpu_threads(),
                eng.n_neurons(), eng.n_synapses(),
                rd_v.active_rows(), rd_mb, ram_mb,
                build_s, cps, drift_kb);
    for (size_t k = 0; k < cols.size(); ++k) {
        std::printf("%s#%d(%.2f)", k ? ", " : "", cols[k].first, cols[k].second);
    }
    std::printf("\n");
    std::fflush(stdout);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Uso:\n"
            "  %s mem   <L1> <L2> <seed>\n"
            "  %s bpc   <corpus> <dic> <L1> <L2> <epocas> <lr> <stdp> <seed>\n"
            "  %s scale <capas_csv> <max_deg> <chars_prueba>\n"
            "  %s macro <capas_csv> <macro_cols> <trn_top_k> <chars_prueba>\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }
    std::string modo = argv[1];
    if (modo == "mem" && argc >= 5) {
        return modo_mem(std::atoi(argv[2]), argv[3], (uint32_t)std::strtoul(argv[4], nullptr, 10));
    }
    if (modo == "bpc" && argc >= 10) {
        return modo_bpc(argv[2], argv[3], std::atoi(argv[4]), argv[5],
                        std::atoi(argv[6]), std::strtof(argv[7], nullptr),
                        std::atoi(argv[8]), (uint32_t)std::strtoul(argv[9], nullptr, 10));
    }
    if (modo == "scale" && argc >= 5) {
        return modo_scale(argv[2], std::atoi(argv[3]), std::strtoull(argv[4], nullptr, 10));
    }
    if (modo == "macro" && argc >= 6) {
        return modo_macro(argv[2], std::atoi(argv[3]), std::atoi(argv[4]),
                          std::strtoull(argv[5], nullptr, 10));
    }
    std::fprintf(stderr, "Argumentos invalidos para modo '%s'\n", modo.c_str());
    return 1;
}
