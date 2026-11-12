// cortex_lm.hpp — nucleo del motor CORTEX-LM, reescrito tras la auditoria
// y extendido a jerarquia cortical multicapa de neuronas LIF puras (CSR + SoA).
//
// POR QUE ESTE ARCHIVO ES ASI
//
// 1. Dinamica LIF auditada en cada capa:
//    - Los pesos se derivan frente a v_th, no se eligen a ciegas.
//    - La entrada sensorial gobierna a la corteza primaria (Capa 0).
//    - Inhibicion proporcional / competicion lateral k-WTA que garantiza punto
//      fijo sin avalanchas ni silencio.
// 2. Escalado lineal O(N) en memoria y O(activos) en computo:
//    - Grafos dispersos CSR intra-capa e inter-capa (InterGraph).
//    - Integracion dirigida por eventos sobre `active_list`: las neuronas en
//      reposo (v = v_reset, theta = 0, refr = 0) no se recorren tick a tick.
//    - Subir de 1.024 a 32.768 o >100.000 neuronas (millones de sinapsis) es
//      solo cambiar `layer_sizes` y `max_out_degree`, sin explotar la RAM.
// 3. Comprension secuencial 100 % con neuronas LIF puras y reglas locales:
//    - Coincidencia dendritica por retardos axonales multi-simbolo (Capa 0 ->
//      Capa 1 -> Capa 2) con saturacion local por rama dendritica (< v_th).
//    - Plasticidad local STDP competitiva (LTP a llegadas coincidentes + poda
//      LTD + homeostasis intrinseca de umbral), sin retropropagacion.
//    - Persistencia binaria completa (guardar/cargar modelo) y aprendizaje
//      continuo con consolidacion sinaptica sobre vocabulario abierto (256 B).

#pragma once

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <iosfwd>
#include <vector>
#include <random>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace cortex {

using i32 = int32_t;
using u32 = uint32_t;

// ---------------------------------------------------------------------------
// Parametros. Unica fuente de verdad.
// ---------------------------------------------------------------------------

struct Params {
    // --- forma y escala ---
    int   n_layers       = 1;      // capas recurrentes (si layer_sizes esta vacio)
    int   layer_size     = 1024;   // neuronas por capa por defecto
    std::vector<int> layer_sizes;  // opcional: tamano explicito por region cortical, ej. {1024, 8192, 8192}
    int   max_out_degree = 0;      // >0 fija grado maximo CSR por neurona -> escala O(N) lineal
    int   n_chars        = 256;    // vocabulario abierto = 256 bytes UTF-8
    float density        = 0.05f;  // densidad cuando max_out_degree == 0
    int   d_max          = 8;      // retardo maximo intra-capa en ticks

    // --- dinamica de membrana ---
    float dt             = 1.0f;
    float tau_m          = 10.0f;  // constante de membrana primaria, en ticks
    float v_th           = 1.0f;   // umbral
    float v_reset        = 0.0f;
    int   refractory     = 1;      // ticks de refractario
    int   ticks_per_char = 8;      // ticks por simbolo (optimo medido 6..8)

    // --- entrada sensorial ---
    float input_gain     = 1.35f;  // corriente base por sinapsis sensorial
    float input_spread   = 0.45f;  // dispersion relativa de esa corriente
    int   input_patch    = 0;      // 0 = derivar del tamano de Capa 0
    float mem_gain       = 3.2f;
    int   mem_patch      = 0;

    // --- recurrencia primaria ---
    float w_scale        = 0.30f;
    float active_frac    = 0.02f;

    // --- inhibicion ---
    float inh_gain       = 0.02f;
    float inh_tau        = 3.0f;

    // --- adaptacion de umbral (fatiga) ---
    float theta_step     = 0.28f;
    float theta_decay    = 0.95f;
    float theta_max      = 1.10f;

    // --- plasticidad clasica de Capa 0 (conservada por compatibilidad) ---
    float eta            = 3e-5f;
    float tau_elig       = 20.0f;
    float eta_slow       = 0.98f;
    float eprop_scale    = 0.125f;
    float w_frac_max     = 0.60f;
    float tau_post       = 12.0f;
    float trace_post_w   = 1.0f;
    float pred_lr        = 0.02f;
    float pred_gain      = 1.0f;
    float bias_homeo_lr  = 0.0f;
    float elig_decay     = 0.90f;
    float elig_clip      = 4.0f;
    float w_min          = 0.0f;
    float w_max          = 3.0f;

    // --- homeostasis lenta ---
    float target_rate    = 0.02f;
    float homeo_lr       = 0.0f;   // 0 = desactivada en Capa 0
    float bias_max       = 0.25f;

    // --- readout clasico ---
    int   n_ctx          = 8;
    float readout_lr     = 8e-3f;
    float readout_norm   = 0.0f;
    float gclip          = 1.0f;

    // --- modulacion dopaminergica (tercer factor opcional) ---
    float dop_gain       = 1.0f;
    float dop_rise       = 0.6f;
    float dop_tau        = 40.0f;
    int   dop_min_len    = 3;
    std::vector<std::string> diccionario;

    // --- jerarquia temporal intra-capa (modo JER 300 clasico) ---
    int   n_fast         = 0;
    float tau_m_fast     = 8.0f;
    float tau_m_slow     = 80.0f;
    float slow_to_fast   = 0.15f;
    int   slow_delay     = 24;
    int   slow_fanout    = 6;
    float slow_in_gain   = 0.45f;

    // --- oscilador interno (desactivado por defecto) ---
    int   n_pace         = 0;
    float pace_gain      = 0.35f;
    float pace_period_a  = 11.0f;
    float pace_period_b  = 29.0f;
    float pace_bias_fast = 1.0f;
    float pace_bias_slow = 0.6f;
    int   pace_lock        = 0;
    float pace_phase_reset = 0.0f;

    // ======================================================================
    // JERARQUIA CORTICAL MULTICAPA DE PULSOS (Capas 0 -> 1 -> 2...)
    // ======================================================================
    // Cuando `cortical_hierarchy = true` y hay >= 2 capas, las regiones
    // superiores son poblaciones de neuronas LIF puras conectadas por grafos
    // dispersos CSR (`InterGraph`) con retardos axonales de varios simbolos.
    // Cada neurona asociativa integra K ramas dendriticas saturadas (< v_th),
    // por lo que SOLO dispara cuando coinciden en el mismo tick los pulsos de
    // la secuencia temporal (silabas, morfemas, palabras y contexto lento).
    bool  cortical_hierarchy   = false;
    float assoc_tau_m          = 1.5f;   // fuga somatica rapida para deteccion de coincidencia
    float assoc_theta_step     = 0.16f;  // fatiga por disparo en capas asociativas
    float assoc_theta_decay    = 0.86f;  // recuperacion de fatiga asociativa
    int   assoc_fan_in_slot    = 5;      // axones candidatos por rama dendritica
    int   assoc_k_wta          = 64;     // ganadores maximos por tick (inhibicion lateral rapida)
    float stdp_ltp             = 0.06f;  // refuerzo STDP local a axones que llegaron a tiempo
    float stdp_ltd             = 0.018f; // poda LTD local a axones silenciosos de la rama
    float homeo_thr_up         = 0.03f;  // subida homeostatica de umbral tras ganar
    float homeo_thr_down       = 0.0004f;// bajada homeostatica suave si nunca dispara
    float homeo_thr_max        = 0.22f;  // banda maxima de umbral homeostatico
    float slow_ctx_frac        = 0.12f;  // fraccion de la capa superior dedicada a memoria lenta
    float slow_ctx_tau_m       = 56.0f;  // constante de membrana lenta (~7 simbolos)
    float slow_ctx_gain        = 0.75f;  // ganancia selectiva desde asambleas de palabras

    // Enrutamiento talamocortical (Nucleo Reticular Talamico, TRN) para macro-escala (>100k..1M+ LIF):
    // cuando `n_macro_cols > 1`, cada region asociativa (li >= 2) se divide en M macro-columnas
    // especializadas; el TRN selecciona las `trn_top_k` mas resonantes e inhibe lateralmente el resto.
    int   n_macro_cols         = 0;
    int   trn_top_k            = 4;

    // --- entrenamiento ---
    int   seq_len        = 128;
    uint32_t seed        = 1234;

    // Derivados.
    int num_layers() const {
        return !layer_sizes.empty() ? (int)layer_sizes.size() : std::max(1, n_layers);
    }
    int size_of_layer(int li) const {
        if (!layer_sizes.empty() && li >= 0 && li < (int)layer_sizes.size())
            return layer_sizes[(size_t)li];
        return layer_size;
    }
    int total_neurons() const {
        int tot = 0;
        const int L = num_layers();
        for (int i = 0; i < L; ++i) tot += size_of_layer(i);
        return tot;
    }
    float alpha_m() const { return std::exp(-dt / tau_m); }
    int   patch_size() const {
        const int n0 = size_of_layer(0);
        return input_patch > 0 ? input_patch : std::max(4, (int)(n0 * 0.06f));
    }
    int   n_ctx_features() const { return n_ctx * n_chars + 1; }
    int   n_features_max() const {
        const int ctx = n_ctx_features();
        const int net = total_neurons();
        return std::max(ctx, net);
    }
};

// Grafo CSR intra-capa por neurona presinaptica: offsets[pre]..offsets[pre+1].
struct Graph {
    std::vector<i32>     offsets;
    std::vector<i32>     post;
    std::vector<float>   weight;
    std::vector<float>   w_init;   // peso inicial (solo se reserva si se usa plasticidad Capa 0)
    std::vector<uint8_t> delay;
    size_t n_syn = 0;
};

// Grafo CSR inter-capa (src_layer -> dst_layer) con retardos axonales multi-simbolo
// y compartimentos dendriticos (0..3 basales + 4 penacho apical Larkum) en `post_slot = (post << 3) | slot`.
struct InterGraph {
    int src_layer = 0;
    int dst_layer = 1;
    std::vector<i32>     offsets;      // N_src + 1
    std::vector<i32>     post_slot;    // (post << 3) | (slot & 7)
    std::vector<float>   weight;       // peso sinaptico actual
    std::vector<uint8_t> delay;        // retardo axonal en ticks (1..255)
    // Vista entrante por neurona postsinaptica para STDP local O(fan_in):
    std::vector<i32>     in_offsets;   // N_dst + 1
    std::vector<i32>     in_syn;       // indice de sinapsis en `weight`/`delay`/`post_slot`
    std::vector<i32>     in_pre;       // neurona presinaptica en `src_layer`
    size_t n_syn = 0;
};

// Estado plano (SoA) de una capa cortical de neuronas LIF.
struct Layer {
    int n = 0;
    int offset = 0;
    int n_assoc = 0;             // primeras n_assoc neuronas son detectoras de coincidencia; el resto lentas

    float tau_m_local      = 10.0f;
    float theta_step_local = 0.28f;
    float theta_decay_local= 0.95f;
    float inh_gain_local   = 0.02f;
    int   k_wta_local      = 0;  // >0 activa competicion lateral rapida

    std::vector<float>   v;           // potencial de membrana somatico
    std::vector<float>   theta;       // fatiga por disparo
    std::vector<float>   homeo_thr;   // umbral homeostatico intrinseco (capas asociativas)
    std::vector<float>   bias;        // sesgo homeostatico
    std::vector<float>   elig;        // elegibilidad Capa 0 (bajo demanda)
    std::vector<float>   rate;        // disparos en el simbolo actual
    std::vector<float>   rate_last;   // disparos del ultimo simbolo cerrado
    std::vector<int>     refr;
    std::vector<int>     spike_count;
    std::vector<uint8_t> active;      // 1 si esta en active_list
    std::vector<i32>     active_list; // neuronas con v>0, theta>0 o refr>0

    // Compartimentos dendriticos (4 ramas basales + 1 penacho apical = 5 * n_assoc) y orden K:
    std::vector<float>   dend_v;      // 5 * n_assoc
    std::vector<uint8_t> dend_order;  // n_assoc (2, 3 o 4 ramas basales requeridas)
    std::vector<uint8_t> dend_touched;// n_assoc
    std::vector<i32>     dend_list;   // neuronas que recibieron pulsos dendriticos este tick
    int                  pending_ticks = 0; // ticks mudos acumulados para decaimiento exacto O(1)

    // Historial circular de los ultimos 4 ticks de disparo por neurona (para STDP exacto O(1)):
    std::vector<i32>     last_spk;    // 4 * n
    std::vector<uint8_t> last_spk_wp; // n

    // Lista dispersa de neuronas que dispararon en el simbolo actual y el cerrado:
    std::vector<i32>     sym_spiked;
    std::vector<uint8_t> sym_spiked_flag;
    std::vector<i32>     last_spiked;

    // Traza sinaptica lenta tipo NMDA con depresion de vesiculas Tsodyks-Markram:
    std::vector<float>   nmda_trace;
    std::vector<uint8_t> nmda_active;
    std::vector<i32>     nmda_list;
    std::vector<i32>     nmda_last_sym;

    // Estado talamocortical por macro-columna (activo si p_.n_macro_cols > 1 en li >= 2):
    std::vector<float>   col_energy;
    std::vector<float>   col_gain;
    std::vector<float>   col_habituation;

    // Trazas clasicas de Capa 0:
    std::vector<float> pre_trace;
    std::vector<float> pre_trace_slow;
    float alpha_slow = 0.0f;
    std::vector<float> post_trace;
    float alpha_post = 0.0f;
    std::vector<float> pred;
    std::vector<float> pred_err;
    float alpha_elig = 0.0f;

    Graph g;
};

// Cola de eventos con retardo axonal (8 bytes exactos por evento en L1 cache).
struct DelayQueue {
    struct Ev { i32 post_slot; float w; };
    std::vector<std::vector<Ev>> buckets;
    int head = 0;
    int span = 0;
    void init(int max_delay) {
        span = std::max(4, max_delay + 4);
        buckets.assign((size_t)span, {});
        head = 0;
    }
    void push(int when, Ev e) {
        buckets[(size_t)(((when % span) + span) % span)].push_back(e);
    }
    std::vector<Ev>& drain(int t) {
        head = ((t % span) + span) % span;
        return buckets[(size_t)head];
    }
};

class Engine {
public:
    explicit Engine(const Params& p = Params{});

    void build_random(unsigned seed);
    void reset_state();
    // Conserva una fraccion atenuada de la memoria de trabajo NMDA entre turnos de dialogo
    // limpiando los transitorios sensoriales rapidos:
    void carry_context(float nmda_retention = 0.42f);

    void feed(int ch);
    void inject_only(int ch);
    void inject_mem(int ch);
    int  step();
    void end_symbol();

    // Extrae el codigo disperso (SDR) de las K neuronas conceptuales NMDA mas activas
    // en las regiones superiores (li >= 2), para indexacion sinaptica hipocampal CA3.
    int  nmda_concept_dim() const;
    void extract_nmda_sdr(std::vector<std::pair<i32, float>>& sdr, int top_k = 32) const;
    // Integracion entorrinal -> giro dentado a lo largo de todo un enunciado:
    // acumula por max-pooling las asambleas L2/L3 que disparan en cualquier posicion
    // de la frase (sin perder las primeras palabras al llegar al signo final).
    void accumulate_episodic_sdr(std::vector<float>& acc_buf) const;
    void finalize_episodic_sdr(const std::vector<float>& acc_buf,
                               std::vector<std::pair<i32, float>>& sdr,
                               int max_k = 192) const;

    // Control retrocompatible: activa/desactiva STDP y homeostasis juntas.
    void set_plasticity(bool enabled) {
        stdp_enabled_ = enabled;
        homeostasis_enabled_ = enabled;
    }
    bool plasticity_enabled() const { return stdp_enabled_ || homeostasis_enabled_; }
    void set_stdp_enabled(bool enabled) { stdp_enabled_ = enabled; }
    void set_homeostasis_enabled(bool enabled) { homeostasis_enabled_ = enabled; }
    bool stdp_enabled() const { return stdp_enabled_; }
    bool homeostasis_enabled() const { return homeostasis_enabled_; }
    // Modo ejecutivo prefrontal (sostiene trazas NMDA durante preguntas largas en via dorsal):
    void set_executive_mode(bool enabled) { executive_mode_ = enabled; }
    bool executive_mode() const { return executive_mode_; }

    // Features clasicas (compatibilidad total con herramientas existentes).
    void ctx_features(std::vector<float>& out) const;
    void rate_features(std::vector<float>& out) const;
    void last_features(std::vector<float>& out) const;
    void theta_features(std::vector<float>& out) const;
    void state_features(std::vector<float>& out) const;
    void full_features(std::vector<float>& out) const;
    void tap_features(std::vector<float>& out, int n_tap) const;
    void tap_hist_reserve(int n);

    // Features dispersas O(activos) para lectura cortical escalable a >100k neuronas:
    // devuelve pares (indice_global, valor_no_cero) sin recorrer neuronas silenciosas.
    int  cortical_feature_dim(int n_l0_taps = 2) const;
    void cortical_sparse_features(std::vector<std::pair<i32, float>>& out,
                                  int n_l0_taps = 2,
                                  float nmda_gain = 0.62f) const;

    void readout(const std::vector<float>& f, float* logits) const;
    void normalize(const std::vector<float>& in, std::vector<float>& out) const;
    void update_norm(const std::vector<float>& f);
    void learn(const float* error, const std::vector<float>& f);
    void error_local(int capa, std::vector<float>& out);
    void stats_error_local(int capa, float& medio, float& maximo) const;

    // Modulacion dopaminergica.
    void  cargar_diccionario(const std::string& ruta);
    void  cargar_bigramas(const std::string& ruta_corpus);
    float juzgar_palabra(int ultimo_simbolo);
    float dopamina() const { return dop_; }
    long  dopamina_positivos() const { return dop_pos_; }
    long  dopamina_negativos() const { return dop_neg_; }
    const std::string& palabra_actual() const { return palabra_; }

    // Jerarquia temporal intra-capa.
    void poblaciones(int& rapidas, int& lentas) const {
        rapidas = p_.n_fast > 0 ? p_.n_fast : (layers_.empty() ? 0 : layers_[0].n);
        lentas  = p_.n_fast > 0 ? std::max(0, (layers_.empty() ? 0 : layers_[0].n) - p_.n_fast) : 0;
    }

    float oscilador(int i) const {
        return i < (int)pace_phase_.size() ? pace_phase_[(size_t)i] : 0.0f;
    }

    // Persistencia binaria completa del motor (pesos CSR, retardos, umbrales).
    void save_engine(std::ostream& os) const;
    void load_engine(std::istream& is);

    long   ticks() const { return t_; }
    long   spikes() const { return spikes_; }
    int    n_neurons() const { return n_neurons_; }
    int    num_layers() const { return (int)layers_.size(); }
    size_t n_synapses() const;
    size_t n_inter_synapses() const;
    const Layer& layer(int i) const { return layers_[(size_t)i]; }
    Layer& layer_mut(int i) { return layers_[(size_t)i]; }
    const std::vector<InterGraph>& inter_graphs() const { return inter_; }
    const Params& params() const { return p_; }
    static int max_cpu_threads();
    // Devuelve las macro-columnas talamocorticales mas activas en la region indicada (-1 = superior):
    std::vector<std::pair<int, float>> active_macro_columns(int layer_idx = -1) const;

private:
    Params p_;
    int n_neurons_ = 0;
    bool stdp_enabled_ = true;
    bool homeostasis_enabled_ = true;
    bool executive_mode_ = false;
    std::vector<Layer> layers_;
    std::vector<InterGraph> inter_;
    std::vector<std::vector<int>> out_inter_by_layer_; // indices en inter_ que salen de cada capa
    std::vector<std::vector<int>> in_inter_by_layer_;  // indices en inter_ que entran a cada capa
    std::vector<DelayQueue> queues_;

    std::vector<float> B_;
    std::vector<float> feedback_;
    std::vector<float> fb_prev_;
    std::vector<int>   sensory_;
    std::vector<float> sensory_w_;
    std::vector<int>   memmap_;
    std::vector<float> memmap_w_;
    std::vector<float> ctx_hist_;
    mutable std::vector<float> R_;
    std::vector<float> mu_, sd_;
    std::vector<float> logits_b_;
    std::vector<float> R_m_, R_v_;
    std::vector<float> logits_;
    long t_ = 0;
    int  tick_in_sym_ = -1;
    int  ch_actual_ = -1;

    // Dopamina.
    float dop_ = 0.0f;
    float dop_evento_ = 0.0f;
    long  dop_pos_ = 0, dop_neg_ = 0;
    std::string palabra_;
    std::unordered_set<std::string> dic_;
    std::unordered_map<uint32_t, float> bigr_;
    float bigr_umbral_ = 0.0f;

    // Via lenta -> rapida plana (SoA).
    std::vector<i32>   slow_src_;
    std::vector<float> slow_w_;
    std::vector<float> slow_buf_flat_; // nf * slow_buf_len_
    int slow_nf_ = 0;
    int slow_buf_len_ = 0;

    // Historial circular de fotos para tap_features y cortical_sparse_features.
    mutable std::vector<std::vector<float>> tap_hist_;
    std::vector<std::vector<std::pair<i32, float>>> l0_sparse_taps_;
    int l0_tap_head_ = 0;

    // Oscilador.
    std::vector<float> pace_phase_;
    std::vector<float> pace_period_;
    std::vector<float> inh_prev_layer_;
    float inh_prev_ = 0.0f;
    long spikes_ = 0;
    long symbols_ = 0;
    long last_consol_ = 0;
    std::mt19937 rng_;
    std::vector<std::pair<float, i32>> cand_;
    std::vector<std::pair<float, i32>> col_cand_[5];

    void build_sensory_map();
    void build_cortical_pathways();
    void ensure_readout_allocated() const;
    void apply_local_stdp(int dst_layer, int post_neuron);
    void flush_assoc_decay(Layer& L, int dt_ticks);
};

// ---------------------------------------------------------------------------
// Proyeccion motora / Lector cortical local disperso (O(activos * 256))
// ---------------------------------------------------------------------------
// - Vocabulario abierto de 256 bytes desde el dia 1 (permite aprendizaje
//   continuo sobre cualquier texto nuevo sin cambiar dimensiones).
// - Disposicion contigua por neurona `W[feat * 256 + byte]`: solo las neuronas
//   que realmente dispararon / estan activas actualizan sus sinapsis de salida.
// - Regla 100 % local (delta por neurona de salida con su propio error) +
//   anclaje de consolidacion (`w_anchor`, `imp`) para no olvidar lo aprendido
//   al seguir entrenando con conocimientos nuevos.
class CorticalReadout {
public:
    static constexpr int kVocab = 256;

    void init(int n_features, float lr = 0.08f, float l2_reg = 0.0f);
    void set_lr(float lr) { lr_ = lr; }
    float lr() const { return lr_; }
    int   n_features() const { return F_; }

    // Calibra la escala por dimension sobre flujo sin etiquetas y la congela.
    void calibrate_step(const std::vector<std::pair<i32, float>>& feats);
    void freeze_calibration();
    bool calibrated() const { return calibrated_; }

    // Paso en flujo: calcula probabilidades sobre 256 bytes y, si learn=true,
    // aplica actualizacion local unicamente sobre las neuronas activas.
    // Devuelve -log2(P(target_byte)) en bits.
    double step(int target_byte,
                const std::vector<std::pair<i32, float>>& feats,
                bool learn,
                float mod = 1.0f);

    // Prediccion pura (rellena probs[256]).
    void predict(const std::vector<std::pair<i32, float>>& feats,
                 double* probs,
                 double temperature = 1.0) const;

    // Consolidacion sinaptica: guarda los pesos actuales como ancla lenta
    // proporcional al uso de cada neurona, protegiendo el conocimiento previo
    // durante futuros entrenamientos continuos o alineacion.
    void consolidate(float strength = 0.02f);
    void scale_weights(float factor);

    // Registra un byte en el vocabulario activo si es nuevo (O(1), sin realocar W_).
    int ensure_byte_slot(int byte_val);
    int active_vocab_size() const { return n_active_; }

    void save(std::ostream& os) const;
    void load(std::istream& is);

private:
    int   F_ = 0;
    int   n_active_ = 0;
    float lr_ = 0.08f;
    float l2_reg_ = 0.0f;
    float consol_pull_ = 0.0f;
    long  cal_steps_ = 0;
    bool  calibrated_ = false;

    std::vector<int>    byte_to_slot_; // 256 (-1 si aun no ha aparecido nunca)
    std::vector<int>    slot_to_byte_; // 0..n_active_-1 -> byte 0..255
    std::vector<float>  W_;        // F_ * 256 (contiguo en slots 0..n_active_-1)
    std::vector<float>  W_anchor_; // pesos consolidados (bajo demanda)
    std::vector<float>  imp_;      // importancia acumulada por neurona F_
    std::vector<float>  scale_;    // escala congelada por neurona F_
    std::vector<double> sq_sum_;   // acumulador de calibracion
    std::vector<float>  b_;        // sesgo de los 256 slots
    mutable std::vector<float>  logits_;
    mutable std::vector<double> probs_;
    mutable std::vector<float>  err_;
    mutable std::vector<std::pair<i32, float>> scaled_feats_;
};

// Subregion hipocampal CA3 pura (asociacion dispersa Hebbiana / Willshaw en 1 disparo):
// conecta neuronas conceptuales NMDA de la corteza (SDR disperso) con celulas engrama
// mediante un grafo disperso invertido O(activos), sin busquedas de texto ni backprop.
class EpisodicCA3 {
public:
    void init(int n_concept_dims);
    int  memorize(const std::vector<std::pair<i32, float>>& sdr, const std::string& trace);
    bool recall(const std::vector<std::pair<i32, float>>& query_sdr,
                std::string& out_trace,
                float min_sim = 0.50f,
                float* out_score = nullptr) const;
    size_t num_engrams() const { return engrams_.size(); }
    const std::string& engram(size_t idx) const { return engrams_[idx]; }

    void save(std::ostream& os) const;
    void load(std::istream& is);

private:
    int dim_ = 0;
    std::vector<std::string> engrams_;
    // Sinapsis dispersas invertidas: para cada neurona conceptual j, lista de (engrama_id, peso_hebbiano)
    std::vector<std::vector<std::pair<i32, float>>> syn_from_concept_;
    mutable std::vector<float> exc_buf_;
    mutable std::vector<i32>   touched_engrams_;
};

// Proyeccion motora compacta y dispersa por filas bajo demanda en FP16 (CXK1):
// Disenada para escalar de 100,000 a >1,000,000+ neuronas LIF sin explotar RAM ni disco.
// Solo reserva filas sinapticas para las neuronas que realmente han disparado durante el
// aprendizaje y almacena pesos + anclas consolidadas en punto flotante IEEE-754 de 16 bits.
class CompactCorticalReadout {
public:
    static constexpr int kDefaultSlots = 128;
    static constexpr int kMaxVocab     = 256;

    void init(int n_features, float lr = 0.08f, int init_slots = kDefaultSlots);
    void set_lr(float lr) { lr_ = lr; }
    float lr() const { return lr_; }
    int   n_features() const { return F_; }
    int   slot_capacity() const { return cap_slots_; }
    size_t active_rows() const { return n_rows_; }
    size_t memory_bytes() const;

    void calibrate_step(const std::vector<std::pair<i32, float>>& feats);
    void freeze_calibration();
    bool calibrated() const { return calibrated_; }

    double step(int target_byte,
                const std::vector<std::pair<i32, float>>& feats,
                bool learn,
                float mod = 1.0f);

    void predict(const std::vector<std::pair<i32, float>>& feats,
                 double* probs,
                 double temperature = 1.0) const;

    void consolidate(float strength = 0.02f);
    void scale_weights(float factor);
    int  ensure_byte_slot(int byte_val);
    int  active_vocab_size() const { return n_active_; }

    void save(std::ostream& os) const;
    void load(std::istream& is);

private:
    int    F_ = 0;
    int    cap_slots_ = kDefaultSlots;
    int    n_active_ = 0;
    size_t n_rows_ = 0;
    float  lr_ = 0.08f;
    float  consol_pull_ = 0.0f;
    long   cal_steps_ = 0;
    bool   calibrated_ = false;
    uint32_t sr_rng_ = 0x9E3779B9u;

    std::vector<int>      byte_to_slot_; // 256
    std::vector<int>      slot_to_byte_; // n_active_
    std::vector<i32>      row_id_;       // F_ (-1 si la neurona jamas ha disparado)
    std::vector<i32>      row_to_feat_;  // n_rows_ -> indice de feature original
    std::vector<uint16_t> W_fp16_;       // n_rows_ * cap_slots_ (pesos FP16)
    std::vector<uint16_t> W_anc_fp16_;   // n_rows_ * cap_slots_ (anclas FP16 bajo demanda)
    std::vector<float>    imp_;          // n_rows_ (importancia por fila activa)
    std::vector<float>    scale_;        // F_ (escala congelada)
    std::vector<float>    sq_sum_;       // F_ (acumulador compacto de calibracion)
    std::vector<float>    b_;            // 256

    mutable std::vector<float>  logits_;
    mutable std::vector<double> probs_;
    mutable std::vector<float>  err_;
    mutable std::vector<std::pair<i32, float>> scaled_feats_;

    i32  ensure_row(i32 feat);
    void expand_slots_to_256();
};

// Guardar y cargar el cerebro completo (Engine + CorticalReadout) en un archivo.
void save_brain(const std::string& path, const Engine& eng, const CorticalReadout& rd);
void load_brain(const std::string& path, Engine& eng, CorticalReadout& rd);

}  // namespace cortex
