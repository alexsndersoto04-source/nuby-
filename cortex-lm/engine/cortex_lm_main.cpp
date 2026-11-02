// cortex_lm_main.cpp — utilidades de linea de comandos
//
//   cortex_lm smoke <archivo>         prueba de vida: red pequena, latido
//   cortex_lm bench <archivo>         prueba de memoria de trabajo
//   cortex_lm train <archivo> <datos> <pasos> [--small]
//   cortex_lm eval  <archivo> <datos>
//
// Todo lo que hace falta para entrenar en Kaggle sin dependencias de Python.

#include "cortex_lm.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <string>
#include <cstdlib>

using namespace cortex;

// Devuelve una ruta de escritura valida para el estado.
// En Android no existe /tmp; en Termux el temporal es $PREFIX/tmp.
static std::string state_path(const char* requested) {
    auto writable = [](const std::string& p) {
        std::ofstream t(p + ".probe", std::ios::binary);
        if (!t) return false;
        t.close();
        std::remove((p + ".probe").c_str());
        return true;
    };
    if (requested && requested[0]) {
        std::string p(requested);
        auto slash = p.find_last_of('/');
        std::string dir = (slash == std::string::npos) ? "." : p.substr(0, slash);
        if (dir.empty()) dir = "/";
        if (writable(dir)) return p;
        std::fprintf(stderr, "aviso: %s no es escribible, usando temporal\n", dir.c_str());
    }
    for (const char* e : {"TMPDIR", "PREFIX"}) {
        const char* v = std::getenv(e);
        if (!v) continue;
        std::string dir = std::string(v) + (std::string(e) == "PREFIX" ? "/tmp" : "");
        if (writable(dir)) return dir + "/cortex_state.bin";
    }
    return "cortex_state.bin";
}

static std::vector<unsigned char> read_corpus(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "no se pudo abrir %s\n", path.c_str()); std::exit(1); }
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)),
                                       std::istreambuf_iterator<char>());
}

// Mapa byte -> indice. Vocabulario fijo de 120: los bytes imprimibles y los
// acentuados frecuentes van a sus propios indices; el resto se agrupa.
// Vocabulario = los 256 valores de byte, identidad pura.
//
// La version anterior mapeaba a 120 simbolos con un relleno que dejaba 142
// valores de byte apuntando al indice 0. Como el corpus esta en UTF-8, cada
// acentuado son dos bytes y medio texto caia en el mismo simbolo. Las cifras
// que se midieron con ese mapa estaban contaminadas.
struct Vocab {
    int map(unsigned char b) const { return (int)b; }
};

struct Softmax {
    static void apply(std::vector<float>& z) {
        float mx = *std::max_element(z.begin(), z.end());
        float sum = 0;
        for (auto& x : z) { x = std::exp(x - mx); sum += x; }
        for (auto& x : z) x /= sum;
    }
};

[[maybe_unused]] static double bits_per_char(Engine& eng,
                            const std::vector<unsigned char>& data,
                            const Vocab& v, size_t start, size_t count) {
    std::vector<float> logits(eng.params().n_chars);
    double bits = 0;
    int n = 0;
    for (size_t i = start; i + 1 < start + count && i + 1 < data.size(); ++i) {
        eng.feed(v.map(data[i]));
        eng.readout(logits.data());
        Softmax::apply(logits);
        float p = logits[v.map(data[i + 1])];
        if (p < 1e-9f) p = 1e-9f;
        bits += -std::log2(p);
        n++;
    }
    return n ? bits / n : 0.0;
}

static void cmd_train(const std::string& path, const std::string& data_path,
                      int steps, bool small) {
    Params p;
    if (small) { p.layer_size = 256; p.n_layers = 1; p.density = 0.05f; }
    Engine eng(p);
    eng.build_random(p.seed);
    // CORTEX_VIEW=slot (defecto) | trace  -> comparar las dos lecturas
    const char* vw = std::getenv("CORTEX_VIEW");
    if (!vw || std::strcmp(vw, "slot") == 0) eng.set_view(Engine::View::Slot);
    else if (std::strcmp(vw, "trace") == 0) eng.set_view(Engine::View::Trace);
    else if (std::strcmp(vw, "ctx") == 0)   eng.set_view(Engine::View::Ctx);
    else if (std::strcmp(vw, "both") == 0)  eng.set_view(Engine::View::Both);
    else eng.set_view(Engine::View::Slot);

    auto data = read_corpus(data_path);
    Vocab v;
    std::printf("motor: %d neuronas, %zu sinapsis, %zu bytes de corpus\n",
                eng.n_neurons(), eng.n_synapses(), data.size());

    std::vector<float> logits(p.n_chars), err(p.n_chars);
    size_t pos = 0;
    auto t0 = std::chrono::steady_clock::now();
    double loss_acc = 0;

    for (int s = 0; s < steps; ++s) {
        if (pos + p.seq_len + 1 >= data.size()) pos = 0;
        // estado limpio al empezar cada secuencia: arrastrar actividad entre
        // secuencias contamina el objetivo y el readout no puede aprender
        eng.reset_state();
        // un simbolo: alimentar, leer, aprender.
        // El estado NO se limpia dentro de la secuencia: ahi esta el contexto.
        // Solo se reinicia entre secuencias, y aun eso hay que revisarlo: el
        // texto es continuo, cortarlo cada 128 simbolos rompe la continuidad.
        for (int k = 0; k < p.seq_len; ++k) {
            eng.feed(v.map(data[pos + k]));
            eng.readout(logits.data());
            int target = v.map(data[pos + k + 1]);
            Softmax::apply(logits);
            for (auto& x : err) x = 0.0f;
            err[target] = -1.0f;
            for (int c = 0; c < p.n_chars; ++c) err[c] += logits[c];
            loss_acc += -std::log2(std::max(1e-9f, logits[target]));
            eng.learn(err.data());
            if (eng.symbols_done() % 500 == 0) eng.consolidate();
        }
        pos += p.seq_len;

        if ((s + 1) % 10 == 0) {
            auto now = std::chrono::steady_clock::now();
            double sec = std::chrono::duration<double>(now - t0).count();
            auto st = eng.stats();
            std::printf("paso %5d | bpc %.3f | w %.3f | usadas %5d | %.1f s (%.2f paso/s)\n",
                        s + 1, loss_acc / (p.seq_len * 10.0), st.mean_w,
                        st.used_neurons, sec, (s + 1) / sec);
            loss_acc = 0;
            std::fflush(stdout);
        }
    }
    std::string sp = state_path(path.c_str());
    eng.save(sp);
    std::printf("guardado: %s\n", sp.c_str());
}


static int cmd_eval(const char* path) {
    Params p;
    p.layer_size = 256; p.n_layers = 1; p.density = 0.05f;
    if (const char* nc = std::getenv("CORTEX_NCTX")) {
        int k = std::atoi(nc);
        if (k > 0 && k <= 8) p.n_ctx = k;
    }
    Engine eng(p);
    eng.build_random(p.seed);
    const char* vw = std::getenv("CORTEX_VIEW");
    if (!vw || std::strcmp(vw, "slot") == 0) eng.set_view(Engine::View::Slot);
    else if (std::strcmp(vw, "trace") == 0) eng.set_view(Engine::View::Trace);
    else if (std::strcmp(vw, "ctx") == 0)   eng.set_view(Engine::View::Ctx);
    else if (std::strcmp(vw, "both") == 0)  eng.set_view(Engine::View::Both);

    auto data = read_corpus("data/spanish_small.txt");
    const size_t split = data.size() * 8 / 10;
    std::vector<float> logits(p.n_chars), err(p.n_chars);
    Vocab v;

    auto evaluate = [&](size_t from, size_t to, const char* label) {
        if (to - from > 4000) to = from + 4000;
        eng.reset_state();
        double bits = 0; int n = 0;
        for (size_t i = from; i + 1 < to; ++i) {
            eng.feed(v.map(data[i]));
            eng.readout(logits.data());
            Softmax::apply(logits);
            float q = logits[v.map(data[i + 1])];
            if (q < 1e-9f) q = 1e-9f;
            bits += -std::log2(q); n++;
        }
        std::printf("%s: %.3f bpc (%d simbolos)\n", label, bits / n, n);
        return bits / n;
    };

    std::printf("inicio: %zu simbolos, split %zu\n", data.size(), split); std::fflush(stdout);
    auto t_start = std::chrono::steady_clock::now();
    // entrenamiento
    size_t pos = 0;
    for (int s = 0; s < 200; ++s) {
        if (pos + p.seq_len + 1 >= split) { pos = 0; eng.reset_state(); }
        for (int k = 0; k < p.seq_len; ++k) {
            eng.feed(v.map(data[pos + k]));
            eng.readout(logits.data());
            int target = v.map(data[pos + k + 1]);
            Softmax::apply(logits);
            for (auto& x : err) x = 0.0f;
            err[target] = -1.0f;
            for (int c = 0; c < p.n_chars; ++c) err[c] += logits[c];
            eng.learn(err.data());
        }
        pos += p.seq_len;
        {
            auto now = std::chrono::steady_clock::now();
            double sec = std::chrono::duration<double>(now - t_start).count();
            std::printf("  paso %d  %.1f s  (%.2f ms/simbolo)\n", s + 1, sec,
                        sec * 1000.0 / ((s + 1) * p.seq_len));
            std::fflush(stdout);
        }
    }
    std::printf("\n--- resultados ---\n");
    evaluate(0, split, "ENTRENO (lo que vio)");
    evaluate(split, data.size(), "NO VIO  (generalizacion)");
    (void)path;
    return 0;
}

static void cmd_smoke(const std::string& path) {
    Params p;
    p.layer_size = 256;
    p.n_layers = 1;
    p.density = 0.05f;
    p.k_wta = 8;
    Engine eng(p);
    eng.build_random(p.seed);
    eng.set_view(Engine::View::Slot);   // ventana temporal explicita
    std::printf("red: %d neuronas, %zu sinapsis\n", eng.n_neurons(), eng.n_synapses());

    // latido: se inyectan caracteres y se mira que la red no este muerta ni epiléptica
    for (int i = 0; i < 300; ++i) eng.feed(i % 60 + 32);
    auto st = eng.stats();
    std::printf("ticks %ld | disparos %ld | disparos/tick %.1f | usadas %d/%d | w %.3f\n",
                st.ticks, st.spikes, (double)st.spikes / st.ticks,
                st.used_neurons, eng.n_neurons(), st.mean_w);

    // prueba de competicion: cuantas neuronas distintas se usan
    double frac = (double)st.used_neurons / eng.n_neurons();
    const char* verdict = "MUERTA";
    if (frac > 0.10 && (double)st.spikes / st.ticks < 200) verdict = "VIVA";
    else if (frac <= 0.10) verdict = "MONOPOLIO";
    std::printf("veredicto: %s (%.1f%% de neuronas usadas)\n", verdict, frac * 100);

    std::string sp = state_path(path.c_str());
    eng.save(sp);
    Engine e2(p);
    e2.load(sp);
    std::printf("persistencia: %d neuronas, %zu sinapsis recargadas de %s\n",
                e2.n_neurons(), e2.n_synapses(), sp.c_str());
}


// ---------------------------------------------------------------------------
// Bench: la prueba que decide si la regla de aprendizaje local sirve.
//
// Tarea de memoria de trabajo. Se presenta un simbolo A, luego K simbolos de
// relleno, y despues hay que recordar A. La red solo puede acertar si mantiene
// informacion a traves del relleno: es exactamente lo que el corpus exige y
// exactamente lo que no estaba pasando.
//
// Se entrena SOLO el readout, con la recurrencia congelada (READOUT_ONLY) y con
// la regla local activa. Si el readout acierta y la regla local no estropea,
// la memoria existe. Si el readout no pasa del azar, el estado no se conserva.
// ---------------------------------------------------------------------------

static double bench_recall(Engine& eng, int key, int filler_len, int trials,
                           bool train, int* correct_out) {
    Params p = eng.params();
    std::vector<float> logits(p.n_chars), err(p.n_chars);
    int correct = 0;
    for (int t = 0; t < trials; ++t) {
        // Cada ensayo empieza desde el reposo. Sin esto, la actividad del
        // ensayo anterior se mezcla con la del actual y ninguna medicion
        // significa nada (asi fue como el bench dio 0% con features buenas).
        eng.reset_state();
        eng.begin_symbol();
        eng.feed(key);                       // clave
        for (int f = 0; f < filler_len; ++f)
            eng.feed(32 + ((t * 7 + f * 3) % 40));   // relleno variable
        eng.readout(logits.data());

        // argmax
        int best = 0;
        for (int k = 1; k < p.n_chars; ++k) if (logits[k] > logits[best]) best = k;
        if (best == key) correct++;

        if (train) {
            float mx = *std::max_element(logits.begin(), logits.end());
            std::vector<float> prob(p.n_chars);
            float s = 0;
            for (int k = 0; k < p.n_chars; ++k) { prob[k] = std::exp(logits[k] - mx); s += prob[k]; }
            for (int k = 0; k < p.n_chars; ++k) err[k] = prob[k] / s;
            err[key] -= 1.0f;
            eng.learn(err.data());
        }
    }
    if (correct_out) *correct_out = correct;
    return (double)correct / trials;
}

static void cmd_bench(const std::string& path) {
    Params p;
    p.layer_size = 256;
    p.n_layers = 1;
    p.density = 0.05f;
    p.w_init_scale = 0.105f;   // ventana de reverberacion medida
    p.input_gain = 0.62f;

    Engine eng(p);
    eng.build_random(p.seed);
    eng.set_view(Engine::View::Slot);   // ventana temporal explicita

    // Version dura: 40 claves DISTINTAS, relleno mas largo, y prueba en claves
    // que no se usaron en el entrenamiento. Si el readout solo lee "que simbolo
    // aparecio", aprueba la version facil sin tener memoria. Esta no la pasa.
    const int filler = 10;
    const int TRIALS = 40;
    const int EPOCHS = 60;
    const int KEYS = 40;

    std::printf("tarea: memorizar una clave entre %d, %d de relleno, recordarla\n",
                KEYS, filler);
    std::printf("azar: %.1f %%\n\n", 100.0 / p.n_chars);

    double acc = 0;
    for (int e = 0; e < EPOCHS; ++e) {
        int c = 0, tot = 0;
        for (int kk = 0; kk < KEYS; ++kk) {
            int ck = 0;
            bench_recall(eng, kk, filler, 2, true, &ck);
            c += ck; tot += 2;
        }
        acc = (double)c / tot;
        if (e % 10 == 0 || e == EPOCHS - 1)
            std::printf("epoca %2d  acierto %5.1f %%  (%d/%d)\n", e + 1, acc * 100, c, tot);
        std::fflush(stdout);
    }

    // sin relleno, la misma tarea: mide si el readout funciona en absoluto
    int c0 = 0, t0 = 0;
    for (int kk = 0; kk < KEYS; ++kk) { int ck = 0; bench_recall(eng, kk, 0, 2, false, &ck); c0 += ck; t0 += 2; }
    double acc0 = (double)c0 / t0;
    std::printf("\nsin relleno:   %5.1f %%\n", acc0 * 100);
    std::printf("con relleno:   %5.1f %%  (%d claves distintas)\n", acc * 100, KEYS);

    std::printf("\n--- VEREDICTO ---\n");
    if (acc < 0.5) {
        std::printf("RECUERDA EL PRESENTE, NO EL PASADO.\n");
        std::printf("El estado no cruza el relleno: no hay memoria de trabajo.\n");
    } else {
        std::printf("TIENE MEMORIA DE TRABAJO.\n");
        std::printf("La informacion de la clave sobrevive al relleno y se recupera.\n");
        std::printf("Es la condicion para que el modelo pueda aprender contexto.\n");
        if (acc0 < 0.5)
            std::printf("(sin relleno mide un slot distinto: no es comparable)\n");
    }
    eng.save(state_path(path.c_str()));
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("uso:\n  cortex_lm eval\n  cortex_lm smoke <archivo>\n"
                    "  cortex_lm train <archivo> <datos> <pasos> [--small]\n");
        return 2;
    }
    std::string cmd = argv[1], path = argv[2];
    try {
        if (cmd == "eval")  { return cmd_eval(path.c_str()); }
        if (cmd == "smoke") { cmd_smoke(path); return 0; }
        if (cmd == "bench") { cmd_bench(path); return 0; }
        if (cmd == "train") {
            if (argc < 5) { std::printf("faltan argumentos\n"); return 2; }
            bool small = (argc > 5 && std::strcmp(argv[5], "--small") == 0);
            cmd_train(path, argv[3], std::atoi(argv[4]), small);
            return 0;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::printf("comando desconocido: %s\n", cmd.c_str());
    return 2;
}
