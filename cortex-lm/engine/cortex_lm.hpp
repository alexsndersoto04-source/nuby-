// cortex_lm.hpp — nucleo del motor CORTEX-LM, reescrito tras la auditoria.
//
// POR QUE ESTE ARCHIVO ES NUEVO
//
// La version anterior tenia una dinamica que no controlaba la entrada:
//  - los pesos de una neurona sumaban 0.80, o sea 1.3x el umbral por tick con
//    tan solo 10 sinapsis entrantes;
//  - la fuga de membrana era 1.7 % por tick (tau_m=20) y la neurona dispara y
//    queda en refractario, de modo que la fuga nunca frenaba nada;
//  - la unica limitacion era un k-WTA que imponia un numero FIJO de ganadores
//    por tick.
// Resultado medido: la actividad crecia sin punto fijo (0,7 -> 162 disparos por
// simbolo) y la respuesta dependia del historial tanto como del simbolo. La red
// era autonoma: la entrada no la gobernaba.
//
// La dinamica correcta es la contraria: los pesos deben ser DEBILES frente al
// umbral, de modo que haga falta la entrada sensorial para disparar, y debe
// haber inhibicion que crezca con la actividad para que el regimen se
// autolimite. Eso es una red de pulsos, no un oscilador.
//
// Reglas de este archivo:
//  1. Ninguna constante magica: todo parametro vive en Params.
//  2. Todo en arrays planos (SoA) para que el escalado sea lineal.
//  3. La topologia es parametrizable: cambiar de escala es cambiar Params.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <vector>
#include <random>
#include <string>

namespace cortex {

// ---------------------------------------------------------------------------
// Parametros. Unica fuente de verdad.
// ---------------------------------------------------------------------------

struct Params {
    // --- forma ---
    int   n_layers       = 1;      // capas recurrentes
    int   layer_size     = 1024;   // neuronas por capa
    int   n_chars        = 256;    // vocabulario = bytes
    float density        = 0.05f;  // conexiones por neurona (grado de salida)
    int   d_max          = 8;      // retardo maximo en ticks

    // --- dinamica de membrana ---
    float dt             = 1.0f;
    float tau_m          = 10.0f;  // constante de membrana, en ticks
    float v_th           = 1.0f;   // umbral
    float v_reset        = 0.0f;
    int   refractory     = 1;      // ticks de refractario
    int   ticks_per_char = 4;      // ticks por simbolo

    // --- entrada sensorial ---
    // Cada simbolo enciende un subconjunto fijo de neuronas de la capa 0 con
    // corriente `input_gain`. El valor se fija contra v_th: un simbolo solo,
    // sin recurrencia, debe poder hacer disparar a su poblacion. Es la
    // condicion que faltaba y la razon de que la entrada no mandara.
    // Cada neurona sensorial recibe su propia corriente: base * (1 - spread/2 ..
    // 1 + spread/2). Con pesos GRADUADOS la respuesta es gradual, y como el
    // umbral sube con la fatiga, la fraccion de la poblacion que dispara
    // codifica cuan reciente fue la estimulacion. Sin gradiente, la poblacion
    // responde en bloque (todo o nada) y la fatiga la apaga entera.
    float input_gain     = 1.35f;  // corriente base por sinapsis sensorial
    float input_spread   = 0.45f;  // dispersion relativa de esa corriente
    int   input_patch    = 0;      // 0 = derivar del tamano de capa
    // Canal de memoria: proyeccion independiente, mas ancha y mas fuerte, para
    // los simbolos que hay que recordar. Su huella de fatiga es mas profunda
    // que la del relleno, y por eso sobrevive mas tiempo.
    float mem_gain       = 3.2f;
    int   mem_patch      = 0;      // 0 = doble del parche sensorial

    // --- recurrencia ---
    // El peso se fija PARA QUE el total esperado sea w_scale * v_th cuando
    // dispara la fraccion activa tipica. No se ajusta a mano: se deriva del
    // grado de entrada, que es lo que hacia falta y no existia.
    // 0.30 es el optimo medido (ver ESTADO.md): con 0 la capa es 1024
    // integradores independientes de su parche sensorial y el techo es el
    // bigrama; con 0.3 la recurrencia aporta 0,11 bpc. Por encima de 0.4 el
    // regimen dispara demasiado (87 disp/simbolo) y el lector empeora; en 0.5
    // ya se pierde toda la ventaja.
    float w_scale        = 0.30f;
    float active_frac    = 0.02f;  // fraccion de la capa activa por tick

    // --- inhibicion ---
    // Divergente, retardada, y sobre todo PROPORCIONAL a la actividad: cada
    // disparo frena a una muestra aleatoria de la capa. Sin esto el regimen
    // no tiene punto fijo y la actividad crece hasta saturar.
    // La inhibicion es una CORRIENTE proporcional a los disparos del tick
    // anterior, no eventos discretos. Como eventos, cada disparo generaba
    // `inh_fanout` llegadas al mismo retardo: con 75 disparos por simbolo eran
    // 450 restas que caian juntas sobre las mismas neuronas y dejaban su
    // potencial en cero antes de decidir. La red no disparaba nunca, y solo
    // funcionaba partiendo de un estado vacio. Medido: 90 neuronas sobre el
    // umbral y 0 disparos.
    float inh_gain       = 0.02f;  // corriente inhibitoria por disparo del tick anterior
    float inh_tau        = 3.0f;   // ticks de persistencia de esa corriente

    // --- adaptacion de umbral (fatiga) ---
    // Mecanismo cortical clasico de memoria de trabajo: la neurona que dispara
    // sube su umbral y tarda en recuperarse. El patron de fatiga codifica que
    // acaba de ocurrir.
    float theta_step     = 0.28f;  // cuanto sube el umbral por disparo
    float theta_decay    = 0.80f;  // cuanto se relaja por tick
    float theta_max      = 1.10f;  // la fatiga debe poder apagar la entrada

    // --- plasticidad (elegibilidad + senal difundida) ---
    float eta            = 3e-5f;
    // Traza de elegibilidad: constante de tiempo en TICKS. Con 20 ticks (~5
    // simbolos) la ventana cubre la memoria corta sin ensuciarse con el pasado
    // lejano. eta_slow filtra el error difundido: la senal de tres factores con
    // error instantaneo es ruido puro y sin filtrar no mueve los pesos.
    float tau_elig       = 20.0f;
    float eta_slow       = 0.98f;
    // Escala de la senal e-prop. R_ esta acotada a +-8; con este factor la
    // senal queda en el mismo orden que la de la matriz B normal (sigma 1).
    float eprop_scale    = 0.125f;
    // Limite de la plasticidad: fraccion del peso inicial que puede moverse.
    // Sin esto los pesos se van a w_max y la red se satura.
    float w_frac_max     = 0.60f;
    float elig_decay     = 0.90f;  // por tick
    float elig_clip      = 4.0f;
    float w_min          = 0.0f;
    float w_max          = 3.0f;

    // --- homeostasis lenta ---
    // Mantiene la actividad media dentro de una banda, actuando sobre el sesgo
    // de cada neurona. Es lo que impide que el regimen derive con el tiempo.
    // DESACTIVADA POR DEFECTO, y el motivo esta medido: con texto real la red
    // se apagaba a 2 disparos por simbolo y solo 16 neuronas de 1024
    // acumulaban fatiga. La homeostasis se defendia del relleno sintetico
    // silenciando a las neuronas sensoriales, que son las unicas que llevan
    // informacion. Se conserva el parametro para poder reactivarla.
    float target_rate    = 0.02f;
    float homeo_lr       = 0.0f;   // 0 = desactivada
    float bias_max       = 0.25f;

    // --- readout ---
    int   n_ctx          = 8;      // simbolos de historial en las features
    float readout_lr     = 8e-3f;
    float readout_norm   = 0.0f;   // 0 = desactivado; recorte por fila opcional
    float gclip          = 1.0f;

    // --- entrenamiento ---
    int   seq_len        = 128;
    uint32_t seed        = 1234;

    // Derivados.
    float alpha_m() const { return std::exp(-dt / tau_m); }
    int   patch_size() const { return input_patch > 0 ? input_patch : std::max(4, (int)(layer_size * 0.06f)); }
    int   n_ctx_features() const { return n_ctx * n_chars + 1; }
    // Dimension del readout: el MAXIMO de todas las vistas, porque la vista se
    // elige en tiempo de ejecucion y el array se reserva una sola vez.
    int   n_features_max() const {
        const int ctx = n_ctx_features();
        const int net = n_layers * layer_size;
        return std::max(ctx, net);
    }
};

using i32 = int32_t;
using u32 = uint32_t;

// Grafo CSR por neurona presinaptica: offsets[pre]..offsets[pre+1].
struct Graph {
    std::vector<i32>     offsets;
    std::vector<i32>     post;
    std::vector<float>   weight;
    std::vector<float>   w_init;   // peso inicial, para acotar la plasticidad
    std::vector<uint8_t> delay;
    size_t n_syn = 0;
};

// Estado de una capa.
struct Layer {
    int n = 0;
    int offset = 0;

    std::vector<float> v;        // potencial de membrana
    std::vector<float> theta;    // fatiga (adaptacion de umbral)
    std::vector<float> bias;     // sesgo homeostatico
    std::vector<float> elig;     // elegibilidad por sinapsis de salida
    std::vector<float> rate;      // disparos en el simbolo actual
    std::vector<float> rate_last; // perfil del ultimo simbolo cerrado
    std::vector<int>   refr;
    std::vector<int>   spike_count;
    std::vector<uint8_t> active; // 1 si esta en la lista de trabajo
    std::vector<i32>   active_list;

    // Traza de disparo presinaptica con retardo (elegibilidad estilo e-prop).
    // pre_trace[pre] = 1 en el tick en que disparo `pre` y decae con
    // alpha_elig = exp(-1/tau_elig) por tick. Se eleva al retardo de la sinapsis
    // para alinearla con la llegada: el peso solo crece si el disparo
    // presinaptico llego a tiempo de contribuir al disparo postsinaptico.
    std::vector<float> pre_trace;
    float alpha_elig = 0.0f;        // factor de decaimiento por tick

    Graph g;
};

// Cola de eventos con retardo (un bucket por retardo).
struct DelayQueue {
    struct Ev { i32 post; i32 syn; };
    std::vector<std::vector<Ev>> buckets;
    int head = 0;
    int span = 0;
    void init(int max_delay) {
        span = max_delay + 2;
        buckets.assign(span, {});
        head = 0;
    }
    void push(int when, Ev e) { buckets[((when % span) + span) % span].push_back(e); }
    std::vector<Ev>& drain(int t) {
        head = ((t % span) + span) % span;
        return buckets[head];
    }
};

class Engine {
public:
    explicit Engine(const Params& p);

    void build_random(unsigned seed);
    void reset_state();

    // Presenta un simbolo completo: inyecta, avanza ticks_per_char, cierra.
    void feed(int ch);
    // Solo la inyeccion sensorial (para diagnostico tick a tick).
    void inject_only(int ch);
    // Inyeccion por el canal de MEMORIA: proyeccion distinta y mas fuerte que la
    // sensorial. Sirve para marcar los simbolos que hay que recordar, de modo
    // que su huella de fatiga sea mas profunda que la del relleno.
    void inject_mem(int ch);
    // Avanza un tick. Devuelve los disparos de ese tick.
    int step();
    void end_symbol();

    // Features de texto: historial sensorial en one-hot.
    void ctx_features(std::vector<float>& out) const;
    // Features de la red: tasa del simbolo actual. Para diagnostico.
    void rate_features(std::vector<float>& out) const;
    void last_features(std::vector<float>& out) const;
    // Features de memoria: fatiga (theta), que codifica lo que acaba de pasar.
    void theta_features(std::vector<float>& out) const;

    void readout(const std::vector<float>& f, float* logits) const;
    void normalize(const std::vector<float>& in, std::vector<float>& out) const;
    void update_norm(const std::vector<float>& f);
    void learn(const float* error, const std::vector<float>& f);

    long   ticks() const { return t_; }
    long   spikes() const { return spikes_; }
    int    n_neurons() const { return n_neurons_; }
    size_t n_synapses() const;
    const Layer& layer(int i) const { return layers_[i]; }
    Layer& layer_mut(int i) { return layers_[i]; }
    const Params& params() const { return p_; }

private:
    Params p_;
    int n_neurons_ = 0;
    std::vector<Layer> layers_;
    std::vector<DelayQueue> queues_;
    std::vector<float> B_;        // retroalimentacion fija (n_neurons x n_chars)
    std::vector<float> feedback_;
    std::vector<float> fb_prev_;   // filtro paso bajo de la senal de aprendizaje
    std::vector<int>   sensory_;    // mapa fijo simbolo -> neuronas de la capa 0
    std::vector<float> sensory_w_;  // corriente graduada de cada conexion sensorial
    std::vector<int>   memmap_;     // mapa fijo simbolo -> neuronas del canal de memoria
    std::vector<float> memmap_w_;
    std::vector<float> ctx_hist_; // n_ctx * n_chars
    std::vector<float> R_;        // readout n_chars x F
    std::vector<float> mu_, sd_;   // normalizacion por dimension de las features
    std::vector<float> logits_b_;  // sesgo del readout
    std::vector<float> R_m_, R_v_;
    std::vector<float> logits_;
    long t_ = 0;
    float inh_prev_ = 0.0f;   // actividad reciente que alimenta el freno
    long spikes_ = 0;
    long symbols_ = 0;
    long last_consol_ = 0;
    std::mt19937 rng_;
    std::vector<std::pair<float, i32>> cand_;

    void build_sensory_map();
};

}  // namespace cortex
