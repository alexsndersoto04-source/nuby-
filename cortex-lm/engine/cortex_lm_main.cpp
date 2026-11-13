// cortex_lm_main.cpp — CLI del motor CORTEX-LM.
//
// Comandos:
//   smoke <archivo>                     latido: la red no esta muerta ni epiléptica
//   audit                               las comprobaciones estructurales del motor
//   eval <archivo>                       entrena 80 %, evalua en el 20 % no visto
//   train <archivo> <datos> <pasos>      entrenamiento con traza de BPC
//   bench <archivo>                      memoria de trabajo: clave + relleno
//
// Variables de entorno:
//   CORTEX_VIEW = ctx | rate | theta    que features usa el readout
//   CORTEX_NCTX = 1..8                  tamano del historial sensorial
//   CORTEX_N, CORTEX_K                  tamano de capa y ganadores por tick

#include "cortex_lm.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <random>

using namespace cortex;

// ---------------------------------------------------------------------------

static std::vector<unsigned char> read_corpus(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(std::string("no se puede abrir ") + path);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

static void softmax(std::vector<float>& z) {
    float mx = *std::max_element(z.begin(), z.end());
    float s = 0.0f;
    for (auto& x : z) { x = std::exp(x - mx); s += x; }
    for (auto& x : z) x /= s;
}

enum class View { Ctx, Rate, Theta, Last };

static View view_from_env() {
    const char* v = std::getenv("CORTEX_VIEW");
    if (!v) return View::Ctx;
    if (std::strcmp(v, "rate") == 0) return View::Rate;
    if (std::strcmp(v, "theta") == 0 || std::strcmp(v, "fatiga") == 0) return View::Theta;
    if (std::strcmp(v, "last") == 0) return View::Last;
    return View::Ctx;
}

static void features(const Engine& e, View v, std::vector<float>& out) {
    switch (v) {
        case View::Rate:  e.rate_features(out);  break;
        case View::Theta: e.theta_features(out); break;
        case View::Last:  e.last_features(out);  break;
        default:          e.ctx_features(out);   break;
    }
}

// Lectura: normaliza, actualiza estadisticas y devuelve logits.
static void infer(Engine& e, View v, std::vector<float>& f, std::vector<float>& logits) {
    features(e, v, f);
    e.update_norm(f);
    std::vector<float> fn;
    e.normalize(f, fn);
    e.readout(fn, logits.data());
}

static Params params_from_env() {
    Params p;
    p.layer_size = 1024;
    p.n_layers = 1;
    if (const char* s = std::getenv("CORTEX_N")) p.layer_size = std::atoi(s);
    if (const char* s = std::getenv("CORTEX_NCTX")) {
        int k = std::atoi(s);
        if (k > 0 && k <= 8) p.n_ctx = k;
    }
    return p;
}

static void apply_env_overrides(Params& p) {
    p.layer_size = 1024;
    p.n_layers = 1;
    if (const char* s = std::getenv("CORTEX_N")) p.layer_size = std::atoi(s);
    if (const char* s = std::getenv("CORTEX_K")) p.active_frac = std::atof(s);
}

// ---------------------------------------------------------------------------
// smoke: latido
// ---------------------------------------------------------------------------
static int cmd_smoke(const std::string&) {
    Params p = params_from_env();
    Engine e(p);
    e.build_random(p.seed);
    std::printf("red: %d neuronas, %zu sinapsis, %d ticks/simbolo\n",
                e.n_neurons(), e.n_synapses(), p.ticks_per_char);

    e.reset_state();
    for (int i = 0; i < 400; ++i) e.feed(i % 60 + 32);

    // actividad por bloques: un regimen sano es estable
    std::printf("\nactividad por bloques de 50 simbolos:\n");
    double first = -1, last = 0;
    for (int b = 0; b < 8; ++b) {
        long s0 = e.spikes();
        for (int c = 0; c < 50; ++c) e.feed((b * 50 + c) % 60 + 32);
        double per = (e.spikes() - s0) / 50.0;
        if (b == 0) first = per;
        last = per;
        std::printf("  bloque %d: %6.1f disparos/simbolo\n", b, per);
    }
    int used = 0;
    for (int j = 0; j < p.layer_size; ++j) if (e.layer(0).spike_count[j] > 0) used++;
    std::printf("\nneuronas usadas: %d/%d\n", used, p.layer_size);

    const double ratio = (first > 0.5) ? last / first : (last > 0.5 ? 99.0 : 1.0);
    if (last < 1.0) {
        std::printf("veredicto: MUERTA (%.1f disparos/simbolo)\n", last);
        return 1;
    }
    if (ratio > 4.0) {
        std::printf("veredicto: INESTABLE (crece de %.1f a %.1f, x%.1f)\n",
                    first, last, ratio);
        return 1;
    }
    std::printf("veredicto: VIVA Y ESTABLE (%.1f disparos/simbolo)\n", last);
    return 0;
}

// ---------------------------------------------------------------------------
// audit: comprobaciones estructurales
// ---------------------------------------------------------------------------
static int cmd_audit(const std::string&) {
    Params p = params_from_env();
    Engine e(p);
    e.build_random(p.seed);
    int fails = 0;

    std::printf("=== AUDITORIA ESTRUCTURAL ===\n\n");
    std::printf("red: %d neuronas, %zu sinapsis, densidad %.2f, d_max %d\n",
                e.n_neurons(), e.n_synapses(), p.density, p.d_max);

    // 1. corriente recurrente frente al umbral
    {
        const auto& L = e.layer(0);
        double wsum = 0; size_t n = 0;
        for (float w : L.g.weight) { wsum += w; n++; }
        const double wmean = wsum / n;
        const double fan_in = p.density * p.layer_size;
        const double active_n = p.active_frac * p.layer_size;
        const double contrib = (fan_in / p.layer_size) * active_n;
        const double current = contrib * wmean;
        std::printf("\n[1] corriente recurrente por tick: %.3f\n", current);
        std::printf("    umbral: %.2f | corriente esperada / umbral: %.2f\n",
                    p.v_th, current / p.v_th);
        if (current / p.v_th > 1.5) {
            std::printf("    FALLA: la red se autoexcita sin entrada\n");
            fails++;
        } else {
            std::printf("    ok: la entrada hace falta para disparar\n");
        }
    }

    // 2. la entrada sola hace disparar
    {
        long total = 0;
        for (int c = 0; c < 32; ++c) {
            e.reset_state();
            e.inject_only(c);
            for (int s = 0; s < p.ticks_per_char; ++s) total += e.step();
        }
        const double per = total / 32.0;
        std::printf("\n[2] un simbolo aislado produce %.1f disparos\n", per);
        if (per < 1.0) { std::printf("    FALLA: la entrada no gobierna la red\n"); fails++; }
        else std::printf("    ok: la entrada gobierna la red\n");
    }

    // 3. regimen estable en secuencia
    {
        e.reset_state();
        for (int c = 0; c < 300; ++c) e.feed(c % 60 + 32);   // calentamiento
        double b[4];
        for (int i = 0; i < 4; ++i) {
            long s0 = e.spikes();
            for (int c = 0; c < 100; ++c) e.feed((i * 100 + c) % 60 + 32);
            b[i] = (e.spikes() - s0) / 100.0;
        }
        std::printf("\n[3] actividad por bloques tras calentar: %.1f %.1f %.1f %.1f\n",
                    b[0], b[1], b[2], b[3]);
        const double drift = b[3] / std::max(0.1, b[0]);
        std::printf("    deriva: x%.2f\n", drift);
        if (drift > 2.0 || b[3] < 0.5) { std::printf("    FALLA: no hay punto fijo\n"); fails++; }
        else std::printf("    ok: regimen estable\n");
    }

    // 4. ¿la respuesta depende del simbolo mas que del historial?
    {
        const auto& L = e.layer(0);
        e.reset_state();
        for (int c = 0; c < 300; ++c) e.feed(c % 60 + 32);
        auto snap = [&](std::vector<float>& out) {
            out.assign(L.theta.begin(), L.theta.end());   // fatiga = la memoria
            float nrm = 0;
            for (float x : out) nrm += x * x;
            nrm = std::sqrt(nrm) + 1e-6f;
            for (float& x : out) x /= nrm;
        };
        auto diff = [](const std::vector<float>& a, const std::vector<float>& b) {
            double d = 0;
            for (size_t i = 0; i < a.size(); ++i) d += std::fabs(a[i] - b[i]);
            return d / a.size();
        };
        // variar el simbolo con el mismo historial, midiendo la fatiga que
        // el simbolo deja inmediatamente despues
        std::vector<std::vector<float>> by_sym(8);
        for (int c = 0; c < 8; ++c) {
            e.reset_state();
            e.inject_mem(c);
            for (int k = 0; k < p.ticks_per_char; ++k) e.step();
            e.end_symbol();
            snap(by_sym[c]);
        }
        e.reset_state();
        double dmax = 0;
        for (int i = 0; i < 8; ++i)
            for (int j = i + 1; j < 8; ++j)
                dmax = std::max(dmax, diff(by_sym[i], by_sym[j]));
        std::printf("\n[4] separacion del perfil de fatiga entre simbolos: %.4f\n", dmax);
        if (dmax < 0.01) { std::printf("    FALLA: la red no distingue simbolos\n"); fails++; }
        else std::printf("    ok: los simbolos producen estados distintos\n");
    }

    // 5. memoria de trabajo: clave -> relleno -> decodificar la clave
    {
        const int NKEYS = 40, FILL = 8, TRIALS = 800;
        std::vector<std::vector<float>> X;
        std::vector<int> Y;
        std::vector<float> ff;
        std::mt19937 rng(11);
        std::uniform_int_distribution<int> ud(0, NKEYS - 1);
        for (int t = 0; t < TRIALS; ++t) {
            e.reset_state();
            const int key = ud(rng), fil = ud(rng);
            e.inject_mem(key);
            for (int k = 0; k < p.ticks_per_char; ++k) e.step();
            e.end_symbol();
            for (int k = 0; k < FILL; ++k) e.feed(200 + fil);
            e.theta_features(ff);
            X.push_back(ff);
            Y.push_back(key);
        }
        // decodificador ridge offline, particion 70/30
        const int D = (int)X[0].size(), NTR = TRIALS * 7 / 10;
        std::vector<double> mu(D, 0), sd(D, 0);
        for (int t = 0; t < NTR; ++t) for (int j = 0; j < D; ++j) mu[j] += X[t][j];
        for (int j = 0; j < D; ++j) mu[j] /= NTR;
        for (int t = 0; t < NTR; ++t) for (int j = 0; j < D; ++j) { double dd = X[t][j] - mu[j]; sd[j] += dd * dd; }
        for (int j = 0; j < D; ++j) sd[j] = std::sqrt(sd[j] / NTR) + 1e-6;
        std::vector<double> M((size_t)D * D, 0.0), B((size_t)D * NKEYS, 0.0);
        for (int t = 0; t < NTR; ++t) for (int j = 0; j < D; ++j) {
            double x = (X[t][j] - mu[j]) / sd[j];
            for (int k = 0; k < D; ++k) M[(size_t)j * D + k] += x * ((X[t][k] - mu[k]) / sd[k]);
            B[(size_t)j * NKEYS + Y[t]] += x;
        }
        double tr = 0; for (int j = 0; j < D; ++j) tr += M[(size_t)j * D + j];
        for (int j = 0; j < D; ++j) M[(size_t)j * D + j] += 1e-3 * tr / D + 1e-6;
        std::vector<double> W((size_t)D * NKEYS, 0.0);
        for (int it = 0; it < 15; ++it) for (int j = 0; j < D; ++j) {
            double dg = M[(size_t)j * D + j]; if (dg <= 0) continue;
            for (int c = 0; c < NKEYS; ++c) {
                double s = B[(size_t)j * NKEYS + c];
                for (int k = 0; k < D; ++k) if (k != j) s -= M[(size_t)j * D + k] * W[(size_t)k * NKEYS + c];
                W[(size_t)j * NKEYS + c] = s / dg;
            }
        }
        int hit = 0;
        for (int t = NTR; t < TRIALS; ++t) {
            int best = 0; double bv = -1e30;
            for (int c = 0; c < NKEYS; ++c) {
                double v = 0;
                for (int j = 0; j < D; ++j) v += ((X[t][j] - mu[j]) / sd[j]) * W[(size_t)j * NKEYS + c];
                if (v > bv) { bv = v; best = c; }
            }
            if (best == Y[t]) hit++;
        }
        const double acc = 100.0 * hit / (TRIALS - NTR);
        std::printf("\n[5] memoria de trabajo (%d claves, %d de relleno): %.1f %%"
                    " (azar %.1f %%)\n", NKEYS, FILL, acc, 100.0 / NKEYS);
        if (acc < 50.0) { std::printf("    FALLA: no retiene la clave\n"); fails++; }
        else std::printf("    ok: retiene la clave a traves del relleno\n");
    }

    // 6. evaluación del lector compacto no debe filtrar el byte objetivo al vocabulario
    {
        CompactCorticalReadout rd;
        rd.init(4, 0.08f);
        rd.ensure_byte_slot('a');
        const int antes = rd.active_vocab_size();
        const std::vector<std::pair<i32, float>> feats = {{0, 1.0f}};
        const double loss = rd.step('z', feats, false, 0.0f);
        const bool solo_lectura = rd.active_vocab_size() == antes;
        const bool piso_correcto = std::isfinite(loss) && std::fabs(loss + std::log2(1e-6)) < 1e-8;
        std::printf("\n[6] lector compacto evalúa byte ausente sin ampliar vocabulario: %.2f bpc\n", loss);
        if (!solo_lectura || !piso_correcto) {
            std::printf("    FALLA: la evaluación modifica vocabulario o puntúa mal el byte ausente\n");
            fails++;
        } else {
            rd.step('z', feats, true, 0.1f);
            if (rd.active_vocab_size() != antes + 1) {
                std::printf("    FALLA: entrenamiento no incorpora el byte nuevo\n");
                fails++;
            } else {
                std::printf("    ok: inferencia inmutable; entrenamiento incorpora byte nuevo\n");
            }
        }
    }

    std::printf("\n=== RESULTADO: %s ===\n", fails ? "FALLOS" : "TODO OK");
    return fails ? 1 : 0;
}

// ---------------------------------------------------------------------------
// eval: entrenamiento en un tramo, evaluacion en otro
// ---------------------------------------------------------------------------
static int cmd_eval(const std::string& path) {
    Params p;
    apply_env_overrides(p);
    Engine e(p);
    e.build_random(p.seed);
    const View view = view_from_env();

    auto data = read_corpus(path.c_str());
    if (data.size() < 1000) throw std::runtime_error("corpus demasiado pequeno");
    const size_t split = data.size() * 8 / 10;
    const int steps = 200;
    const int EVAL_N = 4000;

    std::vector<float> logits(p.n_chars), err(p.n_chars), f;
    auto t0 = std::chrono::steady_clock::now();

    size_t pos = 0;
    for (int s = 0; s < steps; ++s) {
        if (pos + p.seq_len + 1 >= split) pos = 0;
        for (int k = 0; k < p.seq_len; ++k) {
            e.feed(data[pos + k]);
            infer(e, view, f, logits);
            const int target = data[pos + k + 1];
            softmax(logits);
            for (int c = 0; c < p.n_chars; ++c) err[c] = logits[c];
            err[target] -= 1.0f;
            e.learn(err.data(), f);
        }
        pos += p.seq_len;
        if ((s + 1) % 50 == 0) {
            auto now = std::chrono::steady_clock::now();
            double sec = std::chrono::duration<double>(now - t0).count();
            std::printf("  paso %3d  %.0f s\n", s + 1, sec);
            std::fflush(stdout);
        }
    }

    auto evaluate = [&](size_t from, const char* label) {
        e.reset_state();
        for (int c = 0; c < 200; ++c) e.feed(32);      // calentar regimen
        double bits = 0;
        int n = 0;
        for (size_t i = from; i + 1 < data.size() && n < EVAL_N; ++i, ++n) {
            e.feed(data[i]);
            infer(e, view, f, logits);
            softmax(logits);
            float q = logits[data[i + 1]];
            if (q < 1e-9f) q = 1e-9f;
            bits += -std::log2(q);
        }
        std::printf("%s: %.3f bpc (%d simbolos)\n", label, bits / n, n);
        return bits / n;
    };

    std::printf("\n--- resultados (vista %s) ---\n",
                view == View::Ctx ? "historial" : (view == View::Rate ? "tasa" : (view == View::Theta ? "fatiga" : "ultimo simbolo")));
    evaluate(0, "ENTRENO  ");
    evaluate(split, "NO VIO   ");
    return 0;
}

// ---------------------------------------------------------------------------
// train
// ---------------------------------------------------------------------------
static int cmd_train(const char* data_path, int steps) {
    Params p;
    apply_env_overrides(p);
    Engine e(p);
    e.build_random(p.seed);
    const View view = view_from_env();
    auto data = read_corpus(data_path);

    std::vector<float> logits(p.n_chars), err(p.n_chars), f;
    size_t pos = 0;
    double loss = 0;
    int n = 0;
    auto t0 = std::chrono::steady_clock::now();

    for (int s = 0; s < steps; ++s) {
        if (pos + p.seq_len + 1 >= data.size()) pos = 0;
        for (int k = 0; k < p.seq_len; ++k) {
            e.feed(data[pos + k]);
            infer(e, view, f, logits);
            const int target = data[pos + k + 1];
            softmax(logits);
            loss += -std::log2(std::max(1e-9f, logits[target]));
            n++;
            for (int c = 0; c < p.n_chars; ++c) err[c] = logits[c];
            err[target] -= 1.0f;
            e.learn(err.data(), f);
        }
        pos += p.seq_len;
        if ((s + 1) % 10 == 0) {
            auto now = std::chrono::steady_clock::now();
            double sec = std::chrono::duration<double>(now - t0).count();
            std::printf("paso %5d | bpc %.3f | %.1f s (%.1f paso/s)\n",
                        s + 1, loss / n, sec, (s + 1) / sec);
            loss = 0;
            n = 0;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// bench: memoria de trabajo
// ---------------------------------------------------------------------------
static int cmd_bench(const std::string&) {
    Params p = params_from_env();
    Engine e(p);
    e.build_random(p.seed);
    const View view = view_from_env();

    const int NKEYS = 40, FILLER = 10, TRIALS = 80;
    std::vector<float> logits(p.n_chars), err(p.n_chars), f;

    auto run_epoch = [&](bool train) {
        int correct = 0;
        for (int t = 0; t < TRIALS; ++t) {
            e.reset_state();
            const int key = t % NKEYS;
            e.feed(key);
            for (int i = 0; i < FILLER; ++i) e.feed(64 + ((t * 7 + i * 3) % 40));
            infer(e, view, f, logits);
            int best = 0;
            for (int k = 1; k < p.n_chars; ++k) if (logits[k] > logits[best]) best = k;
            if (best == key) correct++;
            if (train) {
                softmax(logits);
                for (int c = 0; c < p.n_chars; ++c) err[c] = logits[c];
                err[key] -= 1.0f;
                e.learn(err.data(), f);
            }
        }
        return 100.0 * correct / TRIALS;
    };

    std::printf("memoria de trabajo: %d claves, %d de relleno, azar %.1f %%\n",
                NKEYS, FILLER, 100.0 / NKEYS);
    for (int ep = 1; ep <= 60; ++ep) {
        double acc = run_epoch(true);
        if (ep == 1 || ep % 10 == 0)
            std::printf("  epoca %2d  acierto %5.1f %%\n", ep, acc);
        std::fflush(stdout);
    }
    std::printf("\n--- VEREDICTO ---\n");
    double acc = run_epoch(false);
    if (acc > 50.0) std::printf("RECUERDA (%.1f %%)\n", acc);
    else std::printf("NO RECUERDA (%.1f %%)\n", acc);
    return acc > 50.0 ? 0 : 1;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("uso:\n"
                    "  cortex_lm audit\n"
                    "  cortex_lm smoke <archivo>\n"
                    "  cortex_lm eval <datos>\n"
                    "  cortex_lm train <datos> <pasos>\n"
                    "  cortex_lm bench <archivo>\n"
                    "\nvariables: CORTEX_VIEW=ctx|rate|theta|last  CORTEX_N=<neuronas>\n"
                    "           CORTEX_NCTX=1..8  CORTEX_K=<fraccion activa>\n");
        return 2;
    }
    std::string cmd = argv[1];
    try {
        if (cmd == "audit") return cmd_audit("");
        if (cmd == "smoke") return cmd_smoke(argc > 2 ? argv[2] : "");
        if (cmd == "eval")  return cmd_eval(argc > 2 ? argv[2] : "");
        if (cmd == "bench") return cmd_bench("");
        if (cmd == "train") {
            if (argc < 4) { std::printf("faltan argumentos\n"); return 2; }
            return cmd_train(argv[2], std::atoi(argv[3]));
        }
        std::printf("comando desconocido: %s\n", cmd.c_str());
        return 2;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
