// cortex_lm.hpp — nucleo del motor CORTEX-LM
//
// Reglas de este archivo:
//
//  1. NADA de constantes magicas en el codigo. Todo parametro vive en Params.
//  2. NADA de contenedores por neurona. Todo en arrays planos (SoA) para que el
//     escalado sea lineal en memoria y la cache se comporte.
//  3. Toda la topologia es parametrizable: el mismo motor hace una red de 4096
//     neuronas o de un millon. Cambiar de escala es cambiar Params.
//  4. El formato binario lleva version y cabecera con todos los parametros.
//
// Si algun dia hay que cambiar la regla de aprendizaje, se cambia LearningRule.
// Si hay que cambiar la neurona, se cambia Dynamics. El resto no se toca.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <vector>
#include <random>
#include <string>

namespace cortex {

// ---------------------------------------------------------------------------
// Parametros. Unica fuente de verdad de la arquitectura.
// ---------------------------------------------------------------------------

struct Params {
    // --- forma de la red ---
    int n_layers      = 2;      // capas recurrentes LIF
    int layer_size    = 4096;   // neuronas por capa
    int n_chars       = 120;    // vocabulario
    float density     = 0.06f;  // fraccion de conexiones dentro de la capa
    int d_max         = 32;     // retardo maximo (ticks)

    // --- dinamica ---
    float dt          = 1.0f;
    float tau_m       = 20.0f;  // constante de membrana
    float v_th        = 1.0f;
    float input_gain  = 1.15f;   // corriente por sinapsis sensorial
    float v_reset     = 0.0f;
    int   refractory  = 2;
    int   ticks_per_char = 5;

    // --- competicion ---
    int   k_wta       = 41;     // ganadores por tick (1 % de 4096)
    float k_wta_frac  = 0.01f;  // si >0, k se deriva del tamano de capa

    // --- plasticidad ---
    float tau_e       = 50.0f;  // vida de la traza de elegibilidad
    float eta         = 3e-4f;  // tasa de aprendizaje
    float elig_clip   = 3.0f;
    int   consol_every = 500;   // simbolos entre ciclos de reposo
    float consol_gain = 0.35f;
    float prune_below = 0.05f;
    float weight_norm = 1.0f;   // suma objetivo de pesos excitatorios entrantes
    float w_init_scale = 0.045f; // escala inicial de pesos

    // --- limites (robustez) ---
    float w_max       = 4.0f;
    int   budget      = 600;    // disparos maximos por ventana (anti-epilepsia)
    int   budget_win  = 400;
    float theta_step  = 0.02f;  // adaptacion de umbral por disparo

    // --- entrenamiento ---
    int   batch       = 8;
    int   seq_len     = 128;
    uint32_t seed     = 1234;

    // Derivados (no se guardan como fuente de verdad).
    int state_per_layer() const { return layer_size; }
    float alpha_m() const { return std::exp(-dt / tau_m); }
    float alpha_e() const { return std::exp(-dt / tau_e); }
    // features del readout: por capa, tasa actual + contexto decaido, + sesgo
    int n_features() const { return 2 * n_layers * layer_size + 1; }
};

// ---------------------------------------------------------------------------
// Estructuras planas. std::vector<T> para portabilidad; con pmr se puede
// cambiar el asignador sin tocar nada mas (arena, mmap, pinned, GPU host).
// ---------------------------------------------------------------------------

using i32 = int32_t;
using u32 = uint32_t;

// Grafo en CSR por neurona presinaptica.
// offsets[pre] .. offsets[pre+1] -> columnas post, pesos, retardos.
struct Graph {
    std::vector<i32>   offsets;   // (n+1)
    std::vector<i32>   post;
    std::vector<float> weight;
    std::vector<uint8_t> delay;   // 1..d_max
    size_t n_syn = 0;
};

// Estado dinamico de una capa.
struct Layer {
    int n = 0;
    int offset = 0;               // indice global de su primera neurona

    std::vector<float> v;         // potencial de membrana
    std::vector<float> theta;     // umbral adaptativo extra
    std::vector<float> vtrace;    // traza de disparo (para elegibilidad)
    std::vector<float> rate;      // disparos acumulados en el simbolo actual
    std::vector<float> ctx;       // promedio decaido (memoria de contexto)
    std::vector<float> elig;      // traza de elegibilidad (por sinapsis de SALIDA)
    std::vector<float> fast;      // pesos rapidos (por sinapsis de salida)
    std::vector<i32>   last_spike;
    std::vector<i32>   refr;
    std::vector<i32>   spent;     // presupuesto gastado en la ventana
    std::vector<i32>   win_start;
    std::vector<i32>   spike_count;

    // CSR de la capa (indices locales)
    Graph g;

    // Listas de activos: solo se procesa lo que puede cambiar. Sin esto el
    // coste por tick es O(todas las sinapsis) y el motor no escala.
    std::vector<i32>     active;        // neuronas con v != v_reset
    std::vector<uint8_t> in_active;
    std::vector<i32>     active_next;
    std::vector<i32>     touch;          // sinapsis con elegibilidad viva
    std::vector<uint8_t> in_touch;
    std::vector<i32>     touch_snap;     // copia para iterar mientras se toca
};

// Entrada sensorial: n_chars x 8 bits.
static constexpr int BITS_PER_CHAR = 8;

// ---------------------------------------------------------------------------
// Motor
// ---------------------------------------------------------------------------

class Engine {
public:
    explicit Engine(const Params& p);

    // --- construccion ---
    void build_random(unsigned seed);
    void load(const std::string& path);
    void save(const std::string& path) const;

    // --- simulacion ---
    // Procesa un caracter: inyecta el patron y avanza ticks_per_char.
    void feed(int ch);
    // Avanza un tick sin inyectar. Devuelve la suma de disparos.
    int step();

    // --- lectura ---
    // Logits del readout para el caracter siguiente, promediados sobre el
    // ultimo caracter procesado. Tamano n_chars.
    void readout(float* logits) const;

    // --- aprendizaje ---
    // error[k] = softmax(logits)[k] - target[k]. Se difunde por B (fija).
    void learn(const float* error);
    void consolidate();
    void begin_symbol();   // limpia acumuladores de tasa
    void reset_state();

    // --- diagnostico ---
    struct Stats {
        long ticks = 0;
        long spikes = 0;
        long symbols = 0;
        float mean_w = 0;
        float mean_v = 0;
        int used_neurons = 0;
    };
    Stats stats() const;
    long symbols_done() const { return symbols_; }

    const Params& params() const { return p_; }
    int n_neurons() const { return p_.n_layers * p_.layer_size; }
    size_t n_synapses() const;

    // Acceso para pruebas y para el binding.
    const Layer& layer(int i) const { return layers_[i]; }
    Layer& layer(int i) { return layers_[i]; }
    const std::vector<float>& sensory() const { return sensory_; }
    const std::vector<float>& feedback() const { return feedback_; }

private:
    // Cola de retardos: un bucket circular por tick. Es miembro, no global,
    // para que dos motores puedan convivir en el mismo proceso (tests, ensembles).
    struct Ev { i32 post; i32 syn; };
    struct DelayQueue {
        std::vector<std::vector<Ev>> buckets;
        int span = 0;
        int head = 0;
        void init(int d_max) {
            span = 1;
            while (span <= d_max + 2) span <<= 1;
            buckets.assign(span, {});
            head = 0;
        }
        void push(int at, Ev e) { buckets[at & (span - 1)].push_back(e); }
        std::vector<Ev>& drain() {
            auto& b = buckets[head & (span - 1)];
            head++;
            return b;
        }
    };

    Params p_;
    std::vector<DelayQueue> queues_;
    std::vector<std::pair<float, int>> cand_;
    std::vector<Layer> layers_;
    std::vector<float> sensory_;    // n_chars * BITS_PER_CHAR
    std::vector<float> feedback_;   // n_neurons (señal de aprendizaje por neurona)
    std::vector<float> B_;          // n_neurons x n_chars, FIJA (no se entrena)
    std::vector<float> logits_;     // n_chars
    std::vector<float> act_now_;    // neuronas que dispararon este tick
    std::vector<float> elig_acc_;   // acumulador de suma_t señal*elig por sinapsis

    // readout (aprendido)
    std::vector<float> R_;          // n_chars x n_features
    std::vector<float> R_m_;        // momento (Adam)
    std::vector<float> R_v_;

    std::mt19937 rng_;

    int t_ = 0;
    long spikes_ = 0;
    long symbols_ = 0;
    int symbols_since_consol_ = 0;

    void propagate(int layer_idx, std::vector<float>& arrivals);
    void k_wta(Layer& L, std::vector<int>& winners);
    void feature_vector(std::vector<float>& out) const;
};

}  // namespace cortex
