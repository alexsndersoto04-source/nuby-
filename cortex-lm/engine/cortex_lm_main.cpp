// cortex_lm_main.cpp — utilidades de linea de comandos
//
//   cortex_lm smoke <archivo>         prueba de vida: red pequena, latido
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
struct Vocab {
    int idx[256];
    int n = 120;
    Vocab() {
        for (int i = 0; i < 256; ++i) idx[i] = -1;
        int k = 0;
        for (int b = 32; b < 127 && k < 100; ++b) idx[b] = k++;
        // acentuados del español en UTF-8 latin-1 range
        for (int b : {10, 9}) idx[b] = k++;
        while (k < n) { // relleno: nunca se usan
            idx[128 + (k % 100)] = k; k++;
        }
    }
    int map(unsigned char b) const {
        int v = idx[b];
        return v < 0 ? 0 : v;
    }
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
        // un simbolo: alimentar, leer, aprender
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

static void cmd_smoke(const std::string& path) {
    Params p;
    p.layer_size = 256;
    p.n_layers = 1;
    p.density = 0.05f;
    p.k_wta = 8;
    Engine eng(p);
    eng.build_random(p.seed);
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

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("uso:\n  cortex_lm smoke <archivo>\n"
                    "  cortex_lm train <archivo> <datos> <pasos> [--small]\n");
        return 2;
    }
    std::string cmd = argv[1], path = argv[2];
    try {
        if (cmd == "smoke") { cmd_smoke(path); return 0; }
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
