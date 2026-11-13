// cortex_lm.cpp — implementacion del nucleo CORTEX-LM:
// neuronas LIF puras, grafos dispersos CSR escalables O(N), integracion
// dirigida por eventos O(activos), coincidencia dendritica multicapa,
// plasticidad local STDP competitiva y persistencia binaria completa.

#include "cortex_lm.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <cctype>
#include <string>
#ifdef _OPENMP
#include <omp.h>
#endif

namespace cortex {

namespace {

// Conversion portable IEEE-754 float32 <-> float16 sin depender de instrucciones especificas de CPU.
inline uint16_t fp32_to_fp16(float f) {
    uint32_t x = 0;
    std::memcpy(&x, &f, sizeof(uint32_t));
    const uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t abs_x = x & 0x7FFFFFFFu;
    if (abs_x >= 0x477FE000u) return (uint16_t)(sign | 0x7BFFu);
    if (abs_x < 0x33000000u) return (uint16_t)sign;
    abs_x += 0x00001000u;
    int32_t exp = (int32_t)((abs_x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = abs_x & 0x007FFFFFu;
    if (exp <= 0) {
        uint32_t full_m = ((x & 0x007FFFFFu) | 0x00800000u);
        int sh = 14 - ((int32_t)((x >> 23) & 0xFFu) - 127 + 15);
        if (sh > 24) return (uint16_t)sign;
        return (uint16_t)(sign | ((full_m + (1u << (sh - 1))) >> sh));
    }
    if (exp >= 31) return (uint16_t)(sign | 0x7BFFu);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

// Redondeo estocastico insesgado FP32 -> FP16 para aprendizaje sinaptico local sin swamping:
inline uint16_t fp32_to_fp16_sr(float f, uint32_t& rng) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    uint32_t x = 0;
    std::memcpy(&x, &f, sizeof(uint32_t));
    const uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t abs_x = x & 0x7FFFFFFFu;
    if (abs_x >= 0x477FE000u) return (uint16_t)(sign | 0x7BFFu);
    if (abs_x < 0x33800000u) {
        const float mag = std::fabs(f) * 16777216.0f; // / 2^-24
        if (mag * 4294967296.0f > (float)rng) return (uint16_t)(sign | 0x0001u);
        return (uint16_t)sign;
    }
    int32_t exp0 = (int32_t)((abs_x >> 23) & 0xFFu) - 127 + 15;
    if (exp0 <= 0) {
        uint32_t full_m = (abs_x & 0x007FFFFFu) | 0x00800000u;
        int sh = 14 - exp0;
        uint32_t mask = (1u << sh) - 1u;
        return (uint16_t)(sign | ((full_m + (rng & mask)) >> sh));
    }
    abs_x += (rng & 0x00001FFFu);
    int32_t exp = (int32_t)((abs_x >> 23) & 0xFFu) - 127 + 15;
    if (exp >= 31) return (uint16_t)(sign | 0x7BFFu);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | ((abs_x & 0x007FFFFFu) >> 13));
}

inline float fp16_to_fp32(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x03FFu;
    uint32_t out = 0;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {
            exp = 1;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x03FFu;
            out = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        out = sign | 0x7F800000u | (mant << 13);
    } else {
        out = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &out, sizeof(float));
    return f;
}

inline float branch_cap_for_order(uint8_t ord, float v_th) {
    if (ord <= 2) return 0.622f * v_th;   // 1 rama = 0.622 < 1; 2 ramas = 1.244 >= 1
    if (ord == 3) return 0.415f * v_th;   // 2 ramas = 0.830 < 1; 3 ramas = 1.245 >= 1
    return 0.311f * v_th;                 // 3 ramas = 0.933 < 1; 4 ramas = 1.244 >= 1
}

template <typename T>
void write_pod(std::ostream& os, const T& v) {
    os.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <typename T>
void read_pod(std::istream& is, T& v) {
    is.read(reinterpret_cast<char*>(&v), sizeof(T));
    if (!is) throw std::runtime_error("archivo de modelo truncado o corrupto");
}

template <typename T>
void write_vec(std::ostream& os, const std::vector<T>& v) {
    uint64_t n = (uint64_t)v.size();
    write_pod(os, n);
    if (n > 0) os.write(reinterpret_cast<const char*>(v.data()), (std::streamsize)(n * sizeof(T)));
}

template <typename T>
void read_vec(std::istream& is, std::vector<T>& v) {
    uint64_t n = 0;
    read_pod(is, n);
    v.resize((size_t)n);
    if (n > 0) {
        is.read(reinterpret_cast<char*>(v.data()), (std::streamsize)(n * sizeof(T)));
        if (!is) throw std::runtime_error("vector truncado al cargar modelo");
    }
}

}  // namespace

Engine::Engine(const Params& p) : p_(p) {
    if (p_.num_layers() < 1) throw std::invalid_argument("num_layers >= 1");
    for (int li = 0; li < p_.num_layers(); ++li) {
        if (p_.size_of_layer(li) < 8) throw std::invalid_argument("layer_size >= 8");
    }
    if (p_.d_max < 1) throw std::invalid_argument("d_max >= 1");
    if (p_.ticks_per_char < 1) throw std::invalid_argument("ticks_per_char >= 1");
    ctx_hist_.assign((size_t)p_.n_ctx * p_.n_chars, 0.0f);
}

void Engine::build_sensory_map() {
    const int N = p_.size_of_layer(0);
    const int patch = p_.patch_size();
    const int tex = std::max(4, patch / 2);
    const int per = patch + tex;
    sensory_.assign((size_t)p_.n_chars * per, 0);
    sensory_w_.assign((size_t)p_.n_chars * per, 0.0f);

    for (int ch = 0; ch < p_.n_chars; ++ch) {
        int* dst = &sensory_[(size_t)ch * per];
        float* dw = &sensory_w_[(size_t)ch * per];
        const int start = (int)((uint64_t)(u32)ch * 7919u % (u32)N);
        for (int i = 0; i < patch; ++i) dst[i] = (start + i) % N;
        std::mt19937 enc((u32)(p_.seed * 2654435761u + (u32)ch * 40503u + 7u));
        for (int k = 0; k < tex; ++k) dst[patch + k] = (int)(enc() % (u32)N);

        std::uniform_real_distribution<float> uu(1.0f - p_.input_spread * 0.5f,
                                                 1.0f + p_.input_spread * 0.5f);
        for (int i = 0; i < per; ++i) dw[i] = p_.input_gain * uu(enc);
    }

    const int mp = p_.mem_patch > 0 ? p_.mem_patch : patch * 2;
    memmap_.assign((size_t)p_.n_chars * mp, 0);
    memmap_w_.assign((size_t)p_.n_chars * mp, 0.0f);
    for (int ch = 0; ch < p_.n_chars; ++ch) {
        int* dst = &memmap_[(size_t)ch * mp];
        float* dw = &memmap_w_[(size_t)ch * mp];
        std::mt19937 enc((u32)(p_.seed * 1103515245u + (u32)ch * 12345u + 991u));
        std::uniform_real_distribution<float> uu(1.0f - p_.input_spread * 0.5f,
                                                 1.0f + p_.input_spread * 0.5f);
        for (int i = 0; i < mp; ++i) {
            dst[i] = (int)(enc() % (u32)N);
            dw[i] = p_.mem_gain * uu(enc);
        }
    }
}

void Engine::build_random(unsigned seed) {
    rng_.seed(seed);
    build_sensory_map();

    const int L = p_.num_layers();
    layers_.clear();
    layers_.resize((size_t)L);
    queues_.clear();
    queues_.resize((size_t)L);
    inh_prev_layer_.assign((size_t)L, 0.0f);

    n_neurons_ = 0;
    for (int li = 0; li < L; ++li) n_neurons_ += p_.size_of_layer(li);

    // En modo clasico de 1 capa pequena mantenemos exactamente la misma
    // secuencia de numeros aleatorios que las actas historicas (audit / verifica).
    // A gran escala o en jerarquia cortical no desperdiciamos cientos de MB en B_ y R_.
    const bool legacy_single = (!p_.cortical_hierarchy && p_.layer_sizes.empty() &&
                                L == 1 && n_neurons_ <= 4096);

    if (legacy_single) {
        feedback_.assign((size_t)n_neurons_, 0.0f);
        fb_prev_.assign((size_t)n_neurons_, 0.0f);
        const int per = std::max(2, p_.n_chars / 16);
        std::uniform_int_distribution<int> chd(0, p_.n_chars - 1);
        std::normal_distribution<float> gauss(0.0f, 1.0f);
        B_.assign((size_t)n_neurons_ * p_.n_chars, 0.0f);
        for (int j = 0; j < n_neurons_; ++j)
            for (int k = 0; k < per; ++k)
                B_[(size_t)j * p_.n_chars + chd(rng_)] = gauss(rng_);
    } else {
        B_.clear();
        feedback_.clear();
        fb_prev_.clear();
    }

    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    int running_offset = 0;

    for (int li = 0; li < L; ++li) {
        Layer& Lr = layers_[(size_t)li];
        const int N = p_.size_of_layer(li);
        Lr.n = N;
        Lr.offset = running_offset;
        running_offset += N;

        const bool is_cortical_assoc = (p_.cortical_hierarchy && li >= 1);
        if (is_cortical_assoc) {
            const bool is_top = (li == L - 1);
            const int n_slow = (is_top && p_.slow_ctx_frac > 0.0f)
                             ? std::max(0, (int)(N * p_.slow_ctx_frac)) : 0;
            Lr.n_assoc = N - n_slow;
            Lr.tau_m_local       = p_.assoc_tau_m;
            Lr.theta_step_local  = p_.assoc_theta_step;
            Lr.theta_decay_local = p_.assoc_theta_decay;
            Lr.inh_gain_local    = 0.0f;
            Lr.k_wta_local       = p_.assoc_k_wta;
        } else {
            Lr.n_assoc           = 0;
            Lr.tau_m_local       = p_.tau_m;
            Lr.theta_step_local  = p_.theta_step;
            Lr.theta_decay_local = p_.theta_decay;
            Lr.inh_gain_local    = p_.inh_gain;
            Lr.k_wta_local       = 0;
        }

        Lr.v.assign((size_t)N, p_.v_reset);
        Lr.theta.assign((size_t)N, 0.0f);
        Lr.bias.assign((size_t)N, 0.0f);
        Lr.rate.assign((size_t)N, 0.0f);
        Lr.rate_last.assign((size_t)N, 0.0f);
        Lr.refr.assign((size_t)N, 0);
        Lr.spike_count.assign((size_t)N, 0);
        Lr.active.assign((size_t)N, 0);
        Lr.active_list.clear();
        Lr.active_list.reserve((size_t)std::min(N, 1024));

        Lr.sym_spiked.clear();
        Lr.sym_spiked_flag.assign((size_t)N, 0);
        Lr.last_spiked.clear();

        Lr.last_spk.assign((size_t)N * 4, -100000);
        Lr.last_spk_wp.assign((size_t)N, 0);

        if (is_cortical_assoc) {
            Lr.homeo_thr.assign((size_t)Lr.n_assoc, 0.0f);
            Lr.dend_v.assign((size_t)Lr.n_assoc * 5, 0.0f);
            Lr.dend_order.assign((size_t)Lr.n_assoc, 3);
            Lr.dend_touched.assign((size_t)Lr.n_assoc, 0);
            Lr.dend_list.clear();
            Lr.dend_list.reserve((size_t)std::min(Lr.n_assoc, 2048));
            Lr.pending_ticks = 0;
            // Todas las regiones asociativas altas (li >= 2: palabras, sintagmas, prefrontal)
            // mantienen traza sinaptica lenta NMDA con depresion Tsodyks-Markram:
            if (li >= 2 && Lr.n_assoc > 0) {
                Lr.nmda_trace.assign((size_t)Lr.n_assoc, 0.0f);
                Lr.nmda_active.assign((size_t)Lr.n_assoc, 0);
                Lr.nmda_last_sym.assign((size_t)Lr.n_assoc, -10000);
                Lr.nmda_list.clear();
                Lr.nmda_list.reserve(1024);
                if (p_.n_macro_cols > 1) {
                    const size_t mc = (size_t)p_.n_macro_cols;
                    Lr.col_energy.assign(mc, 0.0f);
                    Lr.col_gain.assign(mc, 1.0f);
                    Lr.col_habituation.assign(mc, 0.0f);
                }
            }

            // Las capas asociativas superiores reciben su recurrencia / contexto
            // por InterGraph multi-retardo; no gastan sinapsis aleatorias densas
            // salvo que se pida explicitamente.
            Lr.g.offsets.assign((size_t)N + 1, 0);
            Lr.g.post.clear();
            Lr.g.weight.clear();
            Lr.g.w_init.clear();
            Lr.g.delay.clear();
            Lr.g.n_syn = 0;
            queues_[(size_t)li].init(std::max(64, p_.ticks_per_char * 8));
        } else {
            Lr.homeo_thr.clear();
            Lr.dend_v.clear();
            Lr.dend_order.clear();
            Lr.dend_touched.clear();
            Lr.dend_list.clear();

            std::uniform_int_distribution<int> nd(0, N - 1);
            int out_deg = std::max(1, (int)(p_.density * N));
            if (p_.max_out_degree > 0) out_deg = std::min(out_deg, p_.max_out_degree);

            const double fan_in_frac = (double)out_deg / (double)N;
            const double active_n = std::max(1.0, (double)p_.active_frac * (double)N);
            const double contrib = fan_in_frac * active_n;
            const double wnum = (double)p_.w_scale * (double)p_.v_th;
            const float w = (float)(wnum / std::max(1.0, contrib));

            Lr.g.offsets.assign((size_t)N + 1, 0);
            Lr.g.post.clear();
            Lr.g.weight.clear();
            Lr.g.w_init.clear();
            Lr.g.delay.clear();
            Lr.g.post.reserve((size_t)N * out_deg);
            Lr.g.weight.reserve((size_t)N * out_deg);
            if (legacy_single) Lr.g.w_init.reserve((size_t)N * out_deg);
            Lr.g.delay.reserve((size_t)N * out_deg);

            std::uniform_int_distribution<int> dd(1, std::max(1, p_.d_max));
            for (int pre = 0; pre < N; ++pre) {
                Lr.g.offsets[(size_t)pre] = (i32)Lr.g.post.size();
                for (int k = 0; k < out_deg; ++k) {
                    int post = nd(rng_);
                    if (post == pre) continue;
                    Lr.g.post.push_back(post);
                    const float ws = w * (0.6f + 0.8f * uni(rng_));
                    Lr.g.weight.push_back(ws);
                    if (legacy_single) Lr.g.w_init.push_back(ws);
                    Lr.g.delay.push_back((uint8_t)dd(rng_));
                }
            }
            Lr.g.offsets[(size_t)N] = (i32)Lr.g.post.size();
            Lr.g.n_syn = Lr.g.post.size();

            if (legacy_single) {
                Lr.elig.assign(Lr.g.n_syn, 0.0f);
                Lr.pre_trace.assign((size_t)N, 0.0f);
                Lr.alpha_elig = std::exp(-1.0f / std::max(1.0f, p_.tau_elig));
                Lr.post_trace.assign((size_t)N, 0.0f);
                Lr.alpha_post = std::exp(-1.0f / std::max(1.0f, p_.tau_post));
                Lr.pre_trace_slow.assign((size_t)N, 0.0f);
                Lr.alpha_slow = std::exp(-1.0f / std::max(1.0f, p_.tau_elig * 6.0f));
                Lr.pred.assign((size_t)N, 0.0f);
                Lr.pred_err.assign((size_t)N, 0.0f);
            }
            queues_[(size_t)li].init(p_.d_max);
        }
    }

    // Jerarquia intra-capa clasica (modo JER 300) con buffer plano SoA.
    const int N0 = layers_[0].n;
    if (p_.n_fast > 0 && p_.n_fast < N0) {
        const int nf = p_.n_fast;
        const int nslow = N0 - nf;
        const int fan = std::max(1, p_.slow_fanout);
        slow_nf_ = nf;
        slow_src_.assign((size_t)nf * fan, 0);
        slow_w_.assign((size_t)nf * fan, 0.0f);
        std::uniform_int_distribution<int> sd_(0, nslow - 1);
        std::normal_distribution<float> gs(0.0f, 1.0f);
        const float sc = p_.slow_to_fast / std::sqrt((float)fan);
        for (int i = 0; i < nf; ++i)
            for (int k = 0; k < fan; ++k) {
                slow_src_[(size_t)i * fan + k] = nf + sd_(rng_);
                slow_w_[(size_t)i * fan + k] = sc * (0.5f + std::fabs(gs(rng_)));
            }
        slow_buf_len_ = std::max(1, p_.slow_delay);
        slow_buf_flat_.assign((size_t)nf * slow_buf_len_, 0.0f);
    } else {
        slow_nf_ = 0;
        slow_buf_len_ = 0;
        slow_src_.clear();
        slow_w_.clear();
        slow_buf_flat_.clear();
    }

    // Oscilador interno.
    pace_phase_.assign((size_t)std::max(0, p_.n_pace), 0.0f);
    pace_period_.assign((size_t)std::max(0, p_.n_pace), 1.0f);
    if (p_.n_pace > 0) {
        const float base[4] = {p_.pace_period_a, p_.pace_period_b,
                               p_.pace_period_a * 2.7f, p_.pace_period_b * 1.7f};
        for (int i = 0; i < p_.n_pace; ++i)
            pace_period_[(size_t)i] = std::max(2.0f, base[i % 4]);
        for (int i = 0; i < p_.n_pace; ++i)
            pace_phase_[(size_t)i] = (float)i / (float)std::max(1, p_.n_pace);
    }

    dic_.clear();
    for (const auto& w : p_.diccionario) dic_.insert(w);
    dop_ = 0.0f;
    dop_evento_ = 0.0f;
    dop_pos_ = dop_neg_ = 0;
    palabra_.clear();

    if (legacy_single) {
        const int F = p_.n_features_max();
        R_.assign((size_t)p_.n_chars * F, 0.0f);
        R_m_.assign(R_.size(), 0.0f);
        R_v_.assign(R_.size(), 0.0f);
        std::normal_distribution<float> rn(0.0f, 0.01f);
        for (auto& x : R_) x = rn(rng_);
        logits_.assign((size_t)p_.n_chars, 0.0f);
        mu_.assign((size_t)F, 0.0f);
        sd_.assign((size_t)F, 1.0f);
        logits_b_.assign((size_t)p_.n_chars, 0.0f);
    } else {
        R_.clear();
        R_m_.clear();
        R_v_.clear();
    }

    // Construir vias corticales inter-capa CSR si cortical_hierarchy esta activo.
    inter_.clear();
    out_inter_by_layer_.assign((size_t)L, {});
    in_inter_by_layer_.assign((size_t)L, {});
    if (p_.cortical_hierarchy && L >= 2) {
        build_cortical_pathways();
    }

    reset_state();
}

void Engine::build_cortical_pathways() {
    const int L = (int)layers_.size();
    const int tpc = p_.ticks_per_char;
    std::mt19937 crng(p_.seed * 97711u + 31337u);
    std::uniform_real_distribution<float> jitter(0.92f, 1.00f);

    for (int li = 1; li < L; ++li) {
        const int src_li = li - 1;
        const Layer& srcL = layers_[(size_t)src_li];
        Layer& dstL = layers_[(size_t)li];
        const int N_src = srcL.n;
        const int N_dst = dstL.n;
        const int n_assoc = dstL.n_assoc;
        const int n_slow  = N_dst - n_assoc;

        // En las capas asociativas muestreamos de las neuronas asociativas de srcL
        // (en Capa 0 son todas las N_src neuronas sensoriales-recurrentes).
        const int src_pool = (src_li == 0) ? N_src : std::max(1, srcL.n_assoc);

        // Fraccion activa tipica en la fuente por tick de coincidencia:
        // en Capa 0 disparan ~6.3% de neuronas en s=0; en capas asociativas disparan k_wta.
        const double p_src = (src_li == 0)
                           ? 0.063
                           : std::max(0.001, (double)srcL.k_wta_local / (double)src_pool);
        // Derivamos cuantos axones candidatos necesita cada rama dendritica para
        // tener ~32% de cobertura juvenil antes de la poda STDP, independiente de N.
        const int m_slot = (src_li == 0)
                         ? std::max(2, p_.assoc_fan_in_slot)
                         : std::max(6, std::min(48, (int)std::round(0.32 / p_src)));

        // Aristas temporales (pre, post_slot, w, delay) para empaquetar luego en CSR.
        struct TmpEdge { i32 pre; i32 post; uint8_t slot; uint8_t delay; float w; };
        std::vector<TmpEdge> edges;
        edges.reserve((size_t)n_assoc * 3 * m_slot + (size_t)n_slow * 16);

        std::uniform_int_distribution<int> src_dist(0, src_pool - 1);

        int max_d_used = 8;
        for (int j = 0; j < n_assoc; ++j) {
            uint8_t K = 3;
            int delays[4] = {1, 1 + tpc, 1 + 2 * tpc, 1 + 3 * tpc};

            if (li == 1) {
                // Capa 1 (silabas y morfemas desde Capa 0):
                // 25% digramas (K=2), 45% trigramas (K=3), 30% tetragramas (K=4).
                const int bucket = (j * 20) / std::max(1, n_assoc);
                if (bucket < 5) {
                    K = 2;
                    delays[0] = 1;
                    delays[1] = 1 + tpc;
                } else if (bucket < 14) {
                    K = 3;
                    delays[0] = 1;
                    delays[1] = 1 + tpc;
                    delays[2] = 1 + 2 * tpc;
                } else {
                    K = 4;
                    delays[0] = 1;
                    delays[1] = 1 + tpc;
                    delays[2] = 1 + 2 * tpc;
                    delays[3] = 1 + 3 * tpc;
                }
            } else if (li == 2) {
                // Capa 2 (palabras y morfemas largos combinando silabas de Capa 1):
                // combina pulso actual (delay 1) con hace 2, 3 o 4 simbolos (palabras de 4..8 letras).
                const int mode = j % 4;
                if (mode == 0) {
                    K = 2;
                    delays[0] = 1;
                    delays[1] = std::min(250, 1 + 2 * tpc);
                } else if (mode == 1) {
                    K = 2;
                    delays[0] = 1;
                    delays[1] = std::min(250, 1 + 3 * tpc);
                } else if (mode == 2) {
                    K = 2;
                    delays[0] = 1;
                    delays[1] = std::min(250, 1 + 4 * tpc);
                } else {
                    K = 3;
                    delays[0] = 1;
                    delays[1] = std::min(250, 1 + 2 * tpc);
                    delays[2] = std::min(250, 1 + 4 * tpc);
                }
            } else {
                // Capas >= 3 (corteza relacional / prefrontal): enlaza asambleas de palabras
                // a escalas de 3, 5, 7 y 10 simbolos para sintagmas y relaciones causales.
                const int sh = li - 3;
                const int mode = j % 4;
                if (mode == 0) {
                    K = 2;
                    delays[0] = 1;
                    delays[1] = std::min(250, 1 + (3 << sh) * tpc);
                } else if (mode == 1) {
                    K = 2;
                    delays[0] = 1;
                    delays[1] = std::min(250, 1 + (5 << sh) * tpc);
                } else if (mode == 2) {
                    K = 2;
                    delays[0] = 1;
                    delays[1] = std::min(250, 1 + (7 << sh) * tpc);
                } else {
                    K = 2;
                    delays[0] = 1;
                    delays[1] = std::min(250, 1 + (10 << sh) * tpc);
                }
            }

            dstL.dend_order[(size_t)j] = K;
            const float bcap = branch_cap_for_order(K, p_.v_th);

            for (uint8_t sl = 0; sl < K; ++sl) {
                const uint8_t d = (uint8_t)std::min(250, delays[sl]);
                if ((int)d > max_d_used) max_d_used = (int)d;
                for (int m = 0; m < m_slot; ++m) {
                    const i32 pre = src_dist(crng);
                    edges.push_back(TmpEdge{pre, j, sl, d, bcap * jitter(crng)});
                }
            }
        }

        // Subpoblacion de memoria de trabajo cortical lenta: cada neurona lenta
        // escucha 2 asambleas especificas de silabas/palabras de la capa previa y
        // sostiene su huella durante ~6-10 simbolos sin saturarse jamas.
        if (n_slow > 0) {
            const int slow_fan = 2;
            for (int j = n_assoc; j < N_dst; ++j) {
                for (int m = 0; m < slow_fan; ++m) {
                    const i32 pre = src_dist(crng);
                    edges.push_back(TmpEdge{pre, j, 0, 1, 0.75f * jitter(crng)});
                }
            }
        }

        // Construir CSR ordenado por `pre` (para despacho O(fanout) al disparar)
        // y vista entrante por `post` (para STDP local O(fan_in) al ganar).
        InterGraph ig;
        ig.src_layer = src_li;
        ig.dst_layer = li;
        ig.n_syn = edges.size();
        ig.offsets.assign((size_t)N_src + 1, 0);
        for (const auto& e : edges) ig.offsets[(size_t)e.pre + 1]++;
        for (int i = 0; i < N_src; ++i) ig.offsets[(size_t)i + 1] += ig.offsets[(size_t)i];

        ig.post_slot.assign(ig.n_syn, 0);
        ig.weight.assign(ig.n_syn, 0.0f);
        ig.delay.assign(ig.n_syn, 0);

        std::vector<i32> cur = ig.offsets;
        std::vector<i32> edge_syn_idx(ig.n_syn, 0);
        for (size_t ei = 0; ei < edges.size(); ++ei) {
            const auto& e = edges[ei];
            const i32 pos = cur[(size_t)e.pre]++;
            ig.post_slot[(size_t)pos] = (e.post << 3) | (e.slot & 7);
            ig.weight[(size_t)pos]    = e.w;
            ig.delay[(size_t)pos]     = e.delay;
            edge_syn_idx[ei]          = pos;
        }

        // Vista entrante para neuronas asociativas (0..n_assoc-1).
        ig.in_offsets.assign((size_t)N_dst + 1, 0);
        for (const auto& e : edges) {
            if (e.post < n_assoc) ig.in_offsets[(size_t)e.post + 1]++;
        }
        for (int j = 0; j < N_dst; ++j) ig.in_offsets[(size_t)j + 1] += ig.in_offsets[(size_t)j];
        const size_t n_in = (size_t)ig.in_offsets[(size_t)N_dst];
        ig.in_syn.assign(n_in, 0);
        ig.in_pre.assign(n_in, 0);
        std::vector<i32> icur = ig.in_offsets;
        for (size_t ei = 0; ei < edges.size(); ++ei) {
            const auto& e = edges[ei];
            if (e.post >= n_assoc) continue;
            const i32 ipos = icur[(size_t)e.post]++;
            ig.in_syn[(size_t)ipos] = edge_syn_idx[ei];
            ig.in_pre[(size_t)ipos] = e.pre;
        }

        if (max_d_used + 8 > queues_[(size_t)li].span) {
            queues_[(size_t)li].init(max_d_used + 8);
        }
        const int ig_idx = (int)inter_.size();
        inter_.push_back(std::move(ig));
        out_inter_by_layer_[(size_t)src_li].push_back(ig_idx);
        in_inter_by_layer_[(size_t)li].push_back(ig_idx);
    }

    // Proyeccion predictiva descendente en cascada (penacho apical Larkum, slot = 4):
    // En 4 regiones conecta L3 -> L2; en >=5 regiones conecta en cascada L4 -> L3 y L3 -> L2,
    // cerrando el bucle talamocortical-prefrontal sin alucinar en vacio.
    if (L >= 4) {
        for (int src_li = L - 1; src_li >= 3; --src_li) {
            const int dst_li = src_li - 1;
            const Layer& srcL = layers_[(size_t)src_li];
            const Layer& dstL = layers_[(size_t)dst_li];
            const int N_src = srcL.n;
            const int N_dst = dstL.n;
            const int src_pool = std::max(1, srcL.n_assoc);
            const int dst_pool = dstL.n_assoc;
            if (dst_pool <= 0) continue;

            const int m_apical = 10;
            const uint8_t d_ap = (uint8_t)std::max(1, tpc - 1);
            const float w_ap = 0.065f * p_.v_th;
            struct TmpAp { i32 pre; i32 post; float w; };
            std::vector<TmpAp> aedges;
            aedges.reserve((size_t)dst_pool * m_apical);
            std::uniform_int_distribution<int> sdist(0, src_pool - 1);
            for (int post = 0; post < dst_pool; ++post) {
                for (int m = 0; m < m_apical; ++m) {
                    aedges.push_back(TmpAp{sdist(crng), post, w_ap * jitter(crng)});
                }
            }
            InterGraph ig;
            ig.src_layer = src_li;
            ig.dst_layer = dst_li;
            ig.n_syn = aedges.size();
            ig.offsets.assign((size_t)N_src + 1, 0);
            for (const auto& e : aedges) ig.offsets[(size_t)e.pre + 1]++;
            for (int i = 0; i < N_src; ++i) ig.offsets[(size_t)i + 1] += ig.offsets[(size_t)i];

            ig.post_slot.assign(ig.n_syn, 0);
            ig.weight.assign(ig.n_syn, 0.0f);
            ig.delay.assign(ig.n_syn, d_ap);
            std::vector<i32> cur = ig.offsets;
            std::vector<i32> esyn(ig.n_syn, 0);
            for (size_t ei = 0; ei < aedges.size(); ++ei) {
                const auto& e = aedges[ei];
                const i32 pos = cur[(size_t)e.pre]++;
                ig.post_slot[(size_t)pos] = (e.post << 3) | 4; // slot 4 = penacho apical
                ig.weight[(size_t)pos]    = e.w;
                esyn[ei]                  = pos;
            }
            ig.in_offsets.assign((size_t)N_dst + 1, 0);
            for (const auto& e : aedges) ig.in_offsets[(size_t)e.post + 1]++;
            for (int j = 0; j < N_dst; ++j) ig.in_offsets[(size_t)j + 1] += ig.in_offsets[(size_t)j];
            ig.in_syn.assign(aedges.size(), 0);
            ig.in_pre.assign(aedges.size(), 0);
            std::vector<i32> icur = ig.in_offsets;
            for (size_t ei = 0; ei < aedges.size(); ++ei) {
                const auto& e = aedges[ei];
                const i32 ipos = icur[(size_t)e.post]++;
                ig.in_syn[(size_t)ipos] = esyn[ei];
                ig.in_pre[(size_t)ipos] = e.pre;
            }
            if ((int)d_ap + 8 > queues_[(size_t)dst_li].span) {
                queues_[(size_t)dst_li].init((int)d_ap + 8);
            }
            const int ig_idx = (int)inter_.size();
            inter_.push_back(std::move(ig));
            out_inter_by_layer_[(size_t)src_li].push_back(ig_idx);
            in_inter_by_layer_[(size_t)dst_li].push_back(ig_idx);
        }
    }
}

size_t Engine::n_synapses() const {
    size_t s = 0;
    for (const auto& L : layers_) s += L.g.n_syn;
    for (const auto& ig : inter_) s += ig.n_syn;
    return s;
}

size_t Engine::n_inter_synapses() const {
    size_t s = 0;
    for (const auto& ig : inter_) s += ig.n_syn;
    return s;
}

void Engine::reset_state() {
    for (auto& L : layers_) {
        std::fill(L.v.begin(), L.v.end(), p_.v_reset);
        std::fill(L.theta.begin(), L.theta.end(), 0.0f);
        std::fill(L.rate.begin(), L.rate.end(), 0.0f);
        std::fill(L.rate_last.begin(), L.rate_last.end(), 0.0f);
        std::fill(L.refr.begin(), L.refr.end(), 0);
        std::fill(L.active.begin(), L.active.end(), 0);
        L.active_list.clear();
        if (!L.elig.empty()) std::fill(L.elig.begin(), L.elig.end(), 0.0f);
        if (!L.pre_trace.empty()) std::fill(L.pre_trace.begin(), L.pre_trace.end(), 0.0f);
        if (!L.post_trace.empty()) std::fill(L.post_trace.begin(), L.post_trace.end(), 0.0f);
        if (!L.pre_trace_slow.empty()) std::fill(L.pre_trace_slow.begin(), L.pre_trace_slow.end(), 0.0f);
        if (!L.dend_v.empty()) std::fill(L.dend_v.begin(), L.dend_v.end(), 0.0f);
        if (!L.dend_touched.empty()) std::fill(L.dend_touched.begin(), L.dend_touched.end(), 0);
        L.dend_list.clear();
        L.pending_ticks = 0;
        std::fill(L.last_spk.begin(), L.last_spk.end(), -100000);
        std::fill(L.last_spk_wp.begin(), L.last_spk_wp.end(), 0);
        L.sym_spiked.clear();
        std::fill(L.sym_spiked_flag.begin(), L.sym_spiked_flag.end(), 0);
        L.last_spiked.clear();
        if (!L.nmda_trace.empty()) std::fill(L.nmda_trace.begin(), L.nmda_trace.end(), 0.0f);
        if (!L.nmda_active.empty()) std::fill(L.nmda_active.begin(), L.nmda_active.end(), 0);
        if (!L.nmda_last_sym.empty()) std::fill(L.nmda_last_sym.begin(), L.nmda_last_sym.end(), -10000);
        L.nmda_list.clear();
        if (!L.col_energy.empty()) std::fill(L.col_energy.begin(), L.col_energy.end(), 0.0f);
        if (!L.col_gain.empty()) std::fill(L.col_gain.begin(), L.col_gain.end(), 1.0f);
    }
    for (auto& q : queues_) {
        for (auto& b : q.buckets) b.clear();
        q.head = 0;
    }
    std::fill(ctx_hist_.begin(), ctx_hist_.end(), 0.0f);
    if (!feedback_.empty()) std::fill(feedback_.begin(), feedback_.end(), 0.0f);
    if (!fb_prev_.empty()) std::fill(fb_prev_.begin(), fb_prev_.end(), 0.0f);
    if (!slow_buf_flat_.empty()) std::fill(slow_buf_flat_.begin(), slow_buf_flat_.end(), 0.0f);
    for (auto& h : tap_hist_) std::fill(h.begin(), h.end(), 0.0f);
    for (auto& st : l0_sparse_taps_) st.clear();
    l0_tap_head_ = 0;
    std::fill(inh_prev_layer_.begin(), inh_prev_layer_.end(), 0.0f);
    inh_prev_ = 0.0f;
    t_ = 0;
    tick_in_sym_ = -1;
}

void Engine::carry_context(float nmda_retention) {
    const float ret = std::max(0.0f, std::min(0.95f, nmda_retention));
    for (auto& L : layers_) {
        std::fill(L.v.begin(), L.v.end(), p_.v_reset);
        std::fill(L.theta.begin(), L.theta.end(), 0.0f);
        std::fill(L.rate.begin(), L.rate.end(), 0.0f);
        std::fill(L.rate_last.begin(), L.rate_last.end(), 0.0f);
        std::fill(L.refr.begin(), L.refr.end(), 0);
        std::fill(L.active.begin(), L.active.end(), 0);
        L.active_list.clear();
        if (!L.dend_v.empty()) std::fill(L.dend_v.begin(), L.dend_v.end(), 0.0f);
        if (!L.dend_touched.empty()) std::fill(L.dend_touched.begin(), L.dend_touched.end(), 0);
        L.dend_list.clear();
        L.pending_ticks = 0;
        std::fill(L.last_spk.begin(), L.last_spk.end(), -100000);
        std::fill(L.last_spk_wp.begin(), L.last_spk_wp.end(), 0);
        L.sym_spiked.clear();
        std::fill(L.sym_spiked_flag.begin(), L.sym_spiked_flag.end(), 0);
        L.last_spiked.clear();
        if (!L.nmda_trace.empty()) {
            size_t nw = 0;
            for (size_t ni = 0; ni < L.nmda_list.size(); ++ni) {
                const i32 j = L.nmda_list[ni];
                L.nmda_trace[(size_t)j] *= ret;
                if (L.nmda_trace[(size_t)j] >= 0.15f) {
                    L.nmda_list[nw++] = j;
                } else {
                    L.nmda_trace[(size_t)j] = 0.0f;
                    L.nmda_active[(size_t)j] = 0;
                }
            }
            L.nmda_list.resize(nw);
        }
    }
    for (auto& q : queues_) {
        for (auto& b : q.buckets) b.clear();
        q.head = 0;
    }
    for (auto& st : l0_sparse_taps_) st.clear();
    l0_tap_head_ = 0;
    std::fill(inh_prev_layer_.begin(), inh_prev_layer_.end(), 0.0f);
    inh_prev_ = 0.0f;
    t_ = 0;
    tick_in_sym_ = -1;
}

void Engine::flush_assoc_decay(Layer& L, int dt_ticks) {
    if (dt_ticks <= 0 || L.active_list.empty()) return;
    const float th_dec = (dt_ticks == 1) ? L.theta_decay_local
                                         : std::pow(L.theta_decay_local, (float)dt_ticks);
    const float am_a1 = std::exp(-p_.dt / std::max(0.25f, L.tau_m_local));
    const float am_s1 = std::exp(-p_.dt / std::max(1.0f, p_.slow_ctx_tau_m));
    const float am_assoc_dt = (dt_ticks == 1) ? am_a1 : std::pow(am_a1, (float)dt_ticks);
    const float am_slow_dt  = (dt_ticks == 1) ? am_s1 : std::pow(am_s1, (float)dt_ticks);
    const int n_assoc = L.n_assoc;

    size_t write_idx = 0;
    for (size_t ai = 0; ai < L.active_list.size(); ++ai) {
        const int j = L.active_list[ai];
        L.theta[(size_t)j] *= th_dec;
        if (L.theta[(size_t)j] < 1e-4f) L.theta[(size_t)j] = 0.0f;

        const int r = L.refr[(size_t)j];
        if (r >= dt_ticks) {
            L.refr[(size_t)j] = (uint8_t)(r - dt_ticks);
        } else {
            const int rem = dt_ticks - r;
            L.refr[(size_t)j] = 0;
            float amj = 1.0f;
            if (rem == dt_ticks) {
                amj = (j < n_assoc) ? am_assoc_dt : am_slow_dt;
            } else {
                amj = std::pow((j < n_assoc) ? am_a1 : am_s1, (float)rem);
            }
            L.v[(size_t)j] = (L.v[(size_t)j] - p_.v_reset) * amj + p_.v_reset;
            if (L.v[(size_t)j] < 1e-4f) L.v[(size_t)j] = p_.v_reset;
        }
        if (L.v[(size_t)j] > p_.v_reset || L.theta[(size_t)j] > 0.0f || L.refr[(size_t)j] > 0) {
            L.active_list[write_idx++] = j;
        } else {
            L.active[(size_t)j] = 0;
        }
    }
    L.active_list.resize(write_idx);
}

void Engine::inject_only(int ch) {
    if (ch < 0 || ch >= p_.n_chars) throw std::out_of_range("simbolo fuera de rango");
    Layer& L0 = layers_[0];
    const int patch = p_.patch_size();
    const int tex = std::max(4, patch / 2);
    const int per = patch + tex;
    const int* dst = &sensory_[(size_t)ch * per];
    const float* dw = &sensory_w_[(size_t)ch * per];
    for (int i = 0; i < per; ++i) {
        int j = dst[i];
        L0.v[(size_t)j] += dw[i];
        if (!L0.active[(size_t)j]) { L0.active[(size_t)j] = 1; L0.active_list.push_back(j); }
    }
    if (p_.n_fast > 0 && p_.slow_in_gain > 0.0f) {
        const int nf = p_.n_fast;
        const int nslow = L0.n - nf;
        if (nslow > 0) {
            for (int i = 0; i < per; ++i) {
                const int j = nf + (dst[i] % nslow);
                L0.v[(size_t)j] += p_.slow_in_gain * dw[i];
                if (!L0.active[(size_t)j]) { L0.active[(size_t)j] = 1; L0.active_list.push_back(j); }
            }
        }
    }
}

void Engine::inject_mem(int ch) {
    if (ch < 0 || ch >= p_.n_chars) throw std::out_of_range("simbolo fuera de rango");
    Layer& L0 = layers_[0];
    const int mp = p_.mem_patch > 0 ? p_.mem_patch : p_.patch_size() * 2;
    const int* dst = &memmap_[(size_t)ch * mp];
    const float* dw = &memmap_w_[(size_t)ch * mp];
    for (int i = 0; i < mp; ++i) {
        const int j = dst[i];
        L0.v[(size_t)j] += dw[i];
        if (!L0.active[(size_t)j]) { L0.active[(size_t)j] = 1; L0.active_list.push_back(j); }
    }
}

void Engine::apply_local_stdp(int dst_li, int post) {
    if (!stdp_enabled_ || (p_.stdp_ltp <= 0.0f && p_.stdp_ltd <= 0.0f)) return;
    Layer& dstL = layers_[(size_t)dst_li];
    if (post >= dstL.n_assoc) return;

    const uint8_t K = dstL.dend_order[(size_t)post];
    const float bcap = branch_cap_for_order(K, p_.v_th);
    const float w_floor = 0.04f * bcap;

    for (int ig_idx : in_inter_by_layer_[(size_t)dst_li]) {
        InterGraph& ig = inter_[(size_t)ig_idx];
        const Layer& srcL = layers_[(size_t)ig.src_layer];
        const i32 i0 = ig.in_offsets[(size_t)post];
        const i32 i1 = ig.in_offsets[(size_t)post + 1];

        // Primero comprobamos que ramas tuvieron al menos un pulso presinaptico
        // coincidente en t_ (o t_-1), para nunca dejar muda una rama entera.
        uint8_t branch_hit_mask = 0;
        for (i32 idx = i0; idx < i1; ++idx) {
            const i32 s = ig.in_syn[(size_t)idx];
            const i32 pre = ig.in_pre[(size_t)idx];
            const uint8_t sl = (uint8_t)(ig.post_slot[(size_t)s] & 7);
            const i32 t_need = (i32)t_ - (i32)ig.delay[(size_t)s];
            const i32* hist = &srcL.last_spk[(size_t)pre * 4];
            if (hist[0] == t_need || hist[1] == t_need || hist[2] == t_need || hist[3] == t_need) {
                branch_hit_mask |= (uint8_t)(1u << sl);
            }
        }

        const float ap_cap = 0.14f * p_.v_th;
        for (i32 idx = i0; idx < i1; ++idx) {
            const i32 s = ig.in_syn[(size_t)idx];
            const i32 pre = ig.in_pre[(size_t)idx];
            const uint8_t sl = (uint8_t)(ig.post_slot[(size_t)s] & 7);
            if (!(branch_hit_mask & (1u << sl))) continue;
            const float cap_sl = (sl == 4) ? ap_cap : bcap;
            const float flr_sl = (sl == 4) ? (0.04f * ap_cap) : w_floor;

            const i32 t_need = (i32)t_ - (i32)ig.delay[(size_t)s];
            const i32* hist = &srcL.last_spk[(size_t)pre * 4];
            const bool arrived = (hist[0] == t_need || hist[1] == t_need ||
                                  hist[2] == t_need || hist[3] == t_need);
            if (arrived) {
                ig.weight[(size_t)s] = std::min(cap_sl, ig.weight[(size_t)s] + p_.stdp_ltp * cap_sl);
            } else {
                ig.weight[(size_t)s] = std::max(flr_sl, ig.weight[(size_t)s] - p_.stdp_ltd * cap_sl);
            }
        }
    }
}

int Engine::step() {
    const float am = p_.alpha_m();
    int total_fired = 0;
    int fired_l0 = 0;

    // Trazas clasicas de Capa 0 (solo si estan reservadas).
    for (auto& L : layers_) {
        if (!L.pre_trace.empty()) {
            for (auto& tr : L.pre_trace) tr *= L.alpha_elig;
            for (auto& tr : L.post_trace) tr *= L.alpha_post;
            for (auto& tr : L.pre_trace_slow) tr *= L.alpha_slow;
        }
    }

    for (size_t li = 0; li < layers_.size(); ++li) {
        Layer& L = layers_[li];
        DelayQueue& q = queues_[li];
        const bool is_cortical_assoc = (p_.cortical_hierarchy && li >= 1);

        if (!is_cortical_assoc) {
            // ==============================================================
            // CAPA PRIMARIA / MODO CLASICO (exactitud 100 % con audit/verifica)
            // ==============================================================
            std::vector<DelayQueue::Ev>& arrived = q.drain((int)t_);
            for (const auto& ev : arrived) {
                const i32 post = ev.post_slot;
                L.v[(size_t)post] += ev.w;
                if (L.v[(size_t)post] < p_.v_reset) L.v[(size_t)post] = p_.v_reset;
                if (!L.active[(size_t)post]) {
                    L.active[(size_t)post] = 1;
                    L.active_list.push_back(post);
                }
            }
            arrived.clear();

            const float i_inh = L.inh_gain_local * inh_prev_;
            const float am_fast = p_.n_fast > 0
                                ? std::exp(-p_.dt / std::max(0.5f, p_.tau_m_fast)) : am;
            const float am_slow = p_.n_fast > 0
                                ? std::exp(-p_.dt / std::max(0.5f, p_.tau_m_slow)) : am;

            const bool need_dense = (p_.n_fast > 0) || (!pace_phase_.empty()) ||
                                    (p_.homeo_lr > 0.0f) || (!slow_buf_flat_.empty());

            int layer_fired = 0;
            auto fire_primary = [&](int j) {
                L.v[(size_t)j] = p_.v_reset;
                L.refr[(size_t)j] = p_.refractory;
                L.rate[(size_t)j] += 1.0f;
                L.spike_count[(size_t)j]++;
                L.theta[(size_t)j] = std::min(p_.theta_max, L.theta[(size_t)j] + L.theta_step_local);
                if (!L.pre_trace.empty()) {
                    L.pre_trace[(size_t)j] = 1.0f;
                    L.pre_trace_slow[(size_t)j] = 1.0f;
                    L.post_trace[(size_t)j] = 1.0f;
                }
                const uint8_t wp = L.last_spk_wp[(size_t)j] & 3u;
                L.last_spk[(size_t)j * 4 + wp] = (i32)t_;
                L.last_spk_wp[(size_t)j] = (uint8_t)(wp + 1u);
                if (!L.sym_spiked_flag[(size_t)j]) {
                    L.sym_spiked_flag[(size_t)j] = 1;
                    L.sym_spiked.push_back(j);
                }
                layer_fired++;

                for (i32 s = L.g.offsets[(size_t)j]; s < L.g.offsets[(size_t)j + 1]; ++s) {
                    q.push((int)t_ + L.g.delay[(size_t)s],
                           DelayQueue::Ev{L.g.post[(size_t)s], L.g.weight[(size_t)s]});
                }
                // La corteza primaria proyecta la rafaga sensorial de inicio de
                // simbolo (tick_in_sym_ <= 0) hacia las areas asociativas, evitando
                // contaminar los ticks intermedios con eco recurrente difuso.
                if (tick_in_sym_ <= 0) {
                    for (int ig_idx : out_inter_by_layer_[li]) {
                        const InterGraph& ig = inter_[(size_t)ig_idx];
                        DelayQueue& dst_q = queues_[(size_t)ig.dst_layer];
                        for (i32 s = ig.offsets[(size_t)j]; s < ig.offsets[(size_t)j + 1]; ++s) {
                            dst_q.push((int)t_ + ig.delay[(size_t)s],
                                       DelayQueue::Ev{ig.post_slot[(size_t)s], ig.weight[(size_t)s]});
                        }
                    }
                }
            };

            if (need_dense) {
                for (int j = 0; j < L.n; ++j) {
                    L.theta[(size_t)j] *= L.theta_decay_local;
                    if (L.theta[(size_t)j] < 1e-4f) L.theta[(size_t)j] = 0.0f;
                    if (L.refr[(size_t)j] > 0) { L.refr[(size_t)j]--; continue; }
                    const float amj = (p_.n_fast > 0 && j >= p_.n_fast) ? am_slow : am_fast;
                    L.v[(size_t)j] = (L.v[(size_t)j] - p_.v_reset) * amj + p_.v_reset
                                   + L.bias[(size_t)j] - i_inh;
                    if (L.v[(size_t)j] < 1e-4f) L.v[(size_t)j] = p_.v_reset;
                }
                if (!pace_phase_.empty()) {
                    for (size_t i = 0; i < pace_phase_.size(); ++i) {
                        pace_phase_[i] += 1.0f / pace_period_[i];
                        if (pace_phase_[i] >= 1.0f) pace_phase_[i] -= 1.0f;
                        const float d = pace_phase_[i];
                        const float pulso = std::exp(-0.5f * (d - 0.5f) * (d - 0.5f) / 0.0025f);
                        const float amp = p_.pace_gain * pulso;
                        if (amp < 1e-4f) continue;
                        for (int j = 0; j < L.n; ++j)
                            L.v[(size_t)j] += amp * (j < p_.n_fast || p_.n_fast == 0
                                                   ? p_.pace_bias_fast : p_.pace_bias_slow);
                    }
                }
                if (!slow_buf_flat_.empty() && slow_nf_ <= L.n) {
                    const int nf = slow_nf_;
                    const int fan = std::max(1, p_.slow_fanout);
                    const int pos = (int)(t_ % slow_buf_len_);
                    for (int i = 0; i < nf; ++i) {
                        float acc = 0.0f;
                        for (int k = 0; k < fan; ++k) {
                            const int src = slow_src_[(size_t)i * fan + k];
                            if (src < L.n) acc += slow_w_[(size_t)i * fan + k] * L.v[(size_t)src];
                        }
                        slow_buf_flat_[(size_t)i * slow_buf_len_ + pos] = acc;
                    }
                    for (int i = 0; i < nf; ++i)
                        L.v[(size_t)i] += slow_buf_flat_[(size_t)i * slow_buf_len_ + pos];
                }
                for (int j = 0; j < L.n; ++j) {
                    if (L.refr[(size_t)j] > 0) continue;
                    if (L.v[(size_t)j] < p_.v_th + L.theta[(size_t)j]) continue;
                    fire_primary(j);
                }
            } else {
                // Camino O(activos) exacto: las neuronas fuera de active_list
                // tienen v = 0, theta = 0, refr = 0 y bias = 0.
                cand_.clear();
                size_t write_idx = 0;
                const size_t n_act = L.active_list.size();
                for (size_t ai = 0; ai < n_act; ++ai) {
                    const int j = L.active_list[ai];
                    L.theta[(size_t)j] *= L.theta_decay_local;
                    if (L.theta[(size_t)j] < 1e-4f) L.theta[(size_t)j] = 0.0f;
                    if (L.refr[(size_t)j] > 0) {
                        L.refr[(size_t)j]--;
                    } else {
                        L.v[(size_t)j] = (L.v[(size_t)j] - p_.v_reset) * am + p_.v_reset
                                       + L.bias[(size_t)j] - i_inh;
                        if (L.v[(size_t)j] < 1e-4f) L.v[(size_t)j] = p_.v_reset;
                        if (L.v[(size_t)j] >= p_.v_th + L.theta[(size_t)j]) {
                            cand_.push_back({0.0f, j});
                        }
                    }
                    if (L.v[(size_t)j] > p_.v_reset || L.theta[(size_t)j] > 0.0f || L.refr[(size_t)j] > 0) {
                        L.active_list[write_idx++] = j;
                    } else {
                        L.active[(size_t)j] = 0;
                    }
                }
                L.active_list.resize(write_idx);
                // Orden ascendente por indice j para coincidir bit a bit con 0..N-1.
                if (cand_.size() > 1) {
                    std::sort(cand_.begin(), cand_.end(),
                              [](const auto& a, const auto& b) { return a.second < b.second; });
                }
                for (const auto& pr : cand_) {
                    const int j = pr.second;
                    fire_primary(j);
                    if (!L.active[(size_t)j]) {
                        L.active[(size_t)j] = 1;
                        L.active_list.push_back(j);
                    }
                }
            }
            if (li == 0) fired_l0 = layer_fired;
            total_fired += layer_fired;
        } else {
            // ==============================================================
            // REGION CORTICAL ASOCIATIVA + APICAL LARKUM + DECAY EVENT-DRIVEN
            // ==============================================================
            std::vector<DelayQueue::Ev>& arrived = q.drain((int)t_);
            if (arrived.empty()) {
                L.pending_ticks++;
                continue;
            }

            // 1. Poner al dia exactamente los ticks mudos acumulados + este tick.
            flush_assoc_decay(L, L.pending_ticks + 1);
            L.pending_ticks = 0;
            const int n_assoc = L.n_assoc;
            const bool use_trn = (!L.col_gain.empty() && p_.n_macro_cols > 1 && n_assoc > 0);
            const int M_cols = use_trn ? p_.n_macro_cols : 1;

            // 2. Drenar llegadas axonales con inhibicion de derivacion (shunting) del TRN:
            //    las macro-columnas silenciadas por el Nucleo Reticular Talamico (< 0.5)
            //    filtran las aferencias antes de despertar compartimentos dendriticos.
            L.dend_list.clear();
            for (const auto& ev : arrived) {
                const int post = ev.post_slot >> 3;
                const int slot = ev.post_slot & 7;
                if (post < n_assoc) {
                    if (use_trn) {
                        const int mc = (int)(((int64_t)post * M_cols) / n_assoc);
                        if (L.col_gain[(size_t)mc] < 0.50f) continue;
                    }
                    if (!L.dend_touched[(size_t)post]) {
                        L.dend_touched[(size_t)post] = 1;
                        L.dend_list.push_back(post);
                    }
                    L.dend_v[(size_t)post * 5 + slot] += ev.w;
                } else {
                    L.v[(size_t)post] = std::min(1.5f * p_.v_th, L.v[(size_t)post] + ev.w);
                    if (!L.active[(size_t)post]) {
                        L.active[(size_t)post] = 1;
                        L.active_list.push_back(post);
                    }
                }
            }
            arrived.clear();

            // 3. Coincidencia basal + amplificacion de calcio apical tipo Larkum (slot 4)
            //    + ganancia del Nucleo Reticular Talamico (TRN) por macro-columna + OpenMP:
            cand_.clear();
            const float ap_cap = 0.14f * p_.v_th;
            const size_t nd = L.dend_list.size();

            if (nd >= 8192 && max_cpu_threads() >= 4) {
                std::vector<float> exc_tmp(nd, -1.0f);
                #ifdef _OPENMP
                #pragma omp parallel for schedule(static)
                #endif
                for (size_t di = 0; di < nd; ++di) {
                    const i32 post = L.dend_list[di];
                    L.dend_touched[(size_t)post] = 0;
                    float* dv = &L.dend_v[(size_t)post * 5];
                    const uint8_t K = L.dend_order[(size_t)post];
                    const float bcap = branch_cap_for_order(K, p_.v_th);

                    float raw_sum = 0.0f;
                    float sat_sum = 0.0f;
                    for (int sl = 0; sl < 4; ++sl) {
                        const float x = dv[sl];
                        dv[sl] = 0.0f;
                        raw_sum += x;
                        sat_sum += (x > bcap ? bcap : x);
                    }
                    const float ap_raw = dv[4];
                    dv[4] = 0.0f;
                    if (L.refr[(size_t)post] > 0) continue;

                    float apical_boost = 0.0f;
                    if (ap_raw > 0.0f && sat_sum > 0.50f * p_.v_th) {
                        apical_boost = (ap_raw > ap_cap ? ap_cap : ap_raw);
                    }
                    const float trn_g = use_trn
                                      ? L.col_gain[(size_t)(((int64_t)post * M_cols) / n_assoc)]
                                      : 1.0f;
                    const float bonus = std::min(0.022f * p_.v_th, 0.008f * raw_sum);
                    const float v_cand = L.v[(size_t)post] + (sat_sum + apical_boost + bonus) * trn_g;
                    const float eff_th = p_.v_th + L.theta[(size_t)post] + L.homeo_thr[(size_t)post];
                    if (v_cand >= eff_th) {
                        L.v[(size_t)post] = v_cand;
                        exc_tmp[di] = v_cand - eff_th;
                    } else {
                        L.v[(size_t)post] = p_.v_reset;
                    }
                }
                for (size_t di = 0; di < nd; ++di) {
                    if (exc_tmp[di] >= 0.0f) {
                        const i32 post = L.dend_list[di];
                        cand_.push_back({exc_tmp[di], post});
                        if (use_trn) {
                            const int mc = (int)(((int64_t)post * M_cols) / n_assoc);
                            L.col_energy[(size_t)mc] += (1.0f + exc_tmp[di]);
                        }
                    }
                }
            } else {
                for (i32 post : L.dend_list) {
                    L.dend_touched[(size_t)post] = 0;
                    float* dv = &L.dend_v[(size_t)post * 5];
                    const uint8_t K = L.dend_order[(size_t)post];
                    const float bcap = branch_cap_for_order(K, p_.v_th);

                    float raw_sum = 0.0f;
                    float sat_sum = 0.0f;
                    for (int sl = 0; sl < 4; ++sl) {
                        const float x = dv[sl];
                        dv[sl] = 0.0f;
                        raw_sum += x;
                        sat_sum += (x > bcap ? bcap : x);
                    }
                    const float ap_raw = dv[4];
                    dv[4] = 0.0f;
                    if (L.refr[(size_t)post] > 0) continue;

                    if (use_trn && sat_sum > 0.35f * p_.v_th) {
                        const int mc = (int)(((int64_t)post * M_cols) / n_assoc);
                        L.col_energy[(size_t)mc] += sat_sum;
                    }

                    float apical_boost = 0.0f;
                    if (ap_raw > 0.0f && sat_sum > 0.50f * p_.v_th) {
                        apical_boost = (ap_raw > ap_cap ? ap_cap : ap_raw);
                    }

                    const float trn_g = use_trn
                                      ? L.col_gain[(size_t)(((int64_t)post * M_cols) / n_assoc)]
                                      : 1.0f;
                    const float bonus = std::min(0.022f * p_.v_th, 0.008f * raw_sum);
                    const float v_cand = L.v[(size_t)post] + (sat_sum + apical_boost + bonus) * trn_g;
                    const float eff_th = p_.v_th + L.theta[(size_t)post] + L.homeo_thr[(size_t)post];
                    if (v_cand >= eff_th) {
                        L.v[(size_t)post] = v_cand;
                        cand_.push_back({v_cand - eff_th, post});
                    } else {
                        L.v[(size_t)post] = p_.v_reset;
                    }
                }
            }

            // 4. Inhibicion lateral rapida POR COLUMNA CORTICAL (segun orden K=2,3,4):
            //    En la corteza real las interneuronas inhibitorias actuan localmente
            //    dentro de cada columna funcional. Asi las asambleas de digramas (K=2)
            //    jamas ahogan a las de trigramas/silabas (K=3) ni morfemas (K=4).
            if (L.k_wta_local > 0 && !cand_.empty()) {
                for (int ord = 2; ord <= 4; ++ord) col_cand_[ord].clear();
                for (const auto& c : cand_) {
                    const uint8_t ord = std::min<uint8_t>(4, std::max<uint8_t>(2, L.dend_order[(size_t)c.second]));
                    col_cand_[ord].push_back(c);
                }
                int active_cols = 0;
                for (int ord = 2; ord <= 4; ++ord) if (!col_cand_[ord].empty()) active_cols++;
                const int per_col = std::max(8, L.k_wta_local / std::max(1, active_cols));

                cand_.clear();
                for (int ord = 2; ord <= 4; ++ord) {
                    auto& cc = col_cand_[ord];
                    if ((int)cc.size() > per_col) {
                        std::nth_element(cc.begin(), cc.begin() + per_col, cc.end(),
                                         [](const auto& a, const auto& b) {
                                             return (a.first != b.first) ? (a.first > b.first)
                                                                         : (a.second < b.second);
                                         });
                        for (size_t ci = (size_t)per_col; ci < cc.size(); ++ci)
                            L.v[(size_t)cc[ci].second] = p_.v_reset;
                        cc.resize((size_t)per_col);
                    }
                    cand_.insert(cand_.end(), cc.begin(), cc.end());
                }
            }

            for (const auto& win : cand_) {
                const int j = win.second;
                L.v[(size_t)j] = p_.v_reset;
                L.refr[(size_t)j] = p_.refractory;
                L.rate[(size_t)j] += 1.0f;
                L.spike_count[(size_t)j]++;
                L.theta[(size_t)j] = std::min(p_.theta_max, L.theta[(size_t)j] + L.theta_step_local);
                if (homeostasis_enabled_ && p_.homeo_thr_up > 0.0f) {
                    L.homeo_thr[(size_t)j] = std::min(p_.homeo_thr_max,
                                                      L.homeo_thr[(size_t)j] + p_.homeo_thr_up);
                }

                const uint8_t wp = L.last_spk_wp[(size_t)j] & 3u;
                L.last_spk[(size_t)j * 4 + wp] = (i32)t_;
                L.last_spk_wp[(size_t)j] = (uint8_t)(wp + 1u);

                if (!L.sym_spiked_flag[(size_t)j]) {
                    L.sym_spiked_flag[(size_t)j] = 1;
                    L.sym_spiked.push_back(j);
                }
                if (!L.active[(size_t)j]) {
                    L.active[(size_t)j] = 1;
                    L.active_list.push_back(j);
                }

                // STDP local competitivo sobre las ramas dendriticas de la ganadora.
                apply_local_stdp((int)li, j);

                // Propagar el pulso a las regiones corticales siguientes.
                for (int ig_idx : out_inter_by_layer_[li]) {
                    const InterGraph& ig = inter_[(size_t)ig_idx];
                    DelayQueue& dst_q = queues_[(size_t)ig.dst_layer];
                    for (i32 s = ig.offsets[(size_t)j]; s < ig.offsets[(size_t)j + 1]; ++s) {
                        dst_q.push((int)t_ + ig.delay[(size_t)s],
                                   DelayQueue::Ev{ig.post_slot[(size_t)s], ig.weight[(size_t)s]});
                    }
                }
            }
            total_fired += (int)cand_.size();
        }
    }

    // Freno proporcional de Capa 0: usa exclusivamente los disparos de Capa 0
    // para que escalar las regiones corticales superiores jamas altere el punto
    // fijo primario.
    inh_prev_ = inh_prev_ * std::exp(-1.0f / std::max(1.0f, p_.inh_tau)) + (float)fired_l0;

    t_++;
    spikes_ += total_fired;
    return total_fired;
}

void Engine::end_symbol() {
    const float want = p_.target_rate * p_.ticks_per_char;
    for (size_t li = 0; li < layers_.size(); ++li) {
        Layer& L = layers_[li];
        const bool is_cortical_assoc = (p_.cortical_hierarchy && li >= 1);

        if (!is_cortical_assoc && p_.homeo_lr > 0.0f) {
            L.rate_last = L.rate;
            for (int j = 0; j < L.n; ++j) {
                const float diff = want - L.rate[(size_t)j];
                L.bias[(size_t)j] += p_.homeo_lr * diff;
                L.bias[(size_t)j] = std::max(-p_.bias_max, std::min(p_.bias_max, L.bias[(size_t)j]));
                L.rate[(size_t)j] = 0.0f;
            }
            for (i32 j : L.sym_spiked) L.sym_spiked_flag[(size_t)j] = 0;
            L.last_spiked = L.sym_spiked;
            L.sym_spiked.clear();
        } else {
            if (L.pending_ticks > 0) {
                flush_assoc_decay(L, L.pending_ticks);
                L.pending_ticks = 0;
            }
            // Actualizacion dispersa O(disparos) de rate_last y rate (sin recorrer N neuronas mudas).
            for (i32 j : L.last_spiked) L.rate_last[(size_t)j] = 0.0f;
            for (i32 j : L.sym_spiked) {
                L.rate_last[(size_t)j] = L.rate[(size_t)j];
                L.rate[(size_t)j] = 0.0f;
                L.sym_spiked_flag[(size_t)j] = 0;
            }
            L.last_spiked.swap(L.sym_spiked);
            L.sym_spiked.clear();

            // Traza sinaptica lenta NMDA multi-escala + depresion Tsodyks-Markram O(activos):
            // regiones prefrontales/relacionales (li >= 3) retienen contexto mas largo (0.978)
            // mientras regiones lexicas (li == 2) mantienen foco de frase (0.962).
            if (!L.nmda_trace.empty()) {
                const float nmda_dec = (li >= 3) ? 0.976f : 0.960f;
                const float mg_gate = 0.28f; // bloqueo Mg2+: filtra colas debiles y delimitadores agotados
                size_t nw = 0;
                for (size_t ni = 0; ni < L.nmda_list.size(); ++ni) {
                    const i32 j = L.nmda_list[ni];
                    L.nmda_trace[(size_t)j] *= nmda_dec;
                    if (L.nmda_trace[(size_t)j] >= mg_gate) {
                        L.nmda_list[nw++] = j;
                    } else {
                        L.nmda_trace[(size_t)j] = 0.0f;
                        L.nmda_active[(size_t)j] = 0;
                    }
                }
                L.nmda_list.resize(nw);
                const i32 cur_sym = (i32)symbols_;
                for (i32 j : L.last_spiked) {
                    if (j < L.n_assoc) {
                        const int dt_sym = std::max(1, (int)(cur_sym - L.nmda_last_sym[(size_t)j]));
                        L.nmda_last_sym[(size_t)j] = cur_sym;
                        const float vesicle_u = 1.0f - 0.82f * std::exp(-(float)dt_sym / 45.0f);
                        const float next_tr = std::min(1.35f, L.nmda_trace[(size_t)j] + vesicle_u);
                        if (next_tr >= mg_gate) {
                            L.nmda_trace[(size_t)j] = next_tr;
                            if (!L.nmda_active[(size_t)j]) {
                                L.nmda_active[(size_t)j] = 1;
                                L.nmda_list.push_back(j);
                            }
                        }
                    }
                }
            }

            // Relajacion homeostatica periodica cada 64 simbolos en capas asociativas
            // (coste amortizado despreciable: N/64 por simbolo).
            if (is_cortical_assoc && homeostasis_enabled_ && p_.homeo_thr_down > 0.0f &&
                ((symbols_ & 63L) == 63L)) {
                const float step_down = p_.homeo_thr_down * 64.0f;
                for (int j = 0; j < L.n_assoc; ++j) {
                    L.homeo_thr[(size_t)j] = std::max(-0.035f, L.homeo_thr[(size_t)j] - step_down);
                }
            }

            // Enrutamiento del Nucleo Reticular Talamico (TRN) entre macro-columnas:
            // selecciona las `trn_top_k` columnas con mayor resonancia neta (energia + NMDA - fatiga)
            // para amplificar el dominio activo e inhibir lateralmente columnas no pertinentes.
            if (!L.col_gain.empty() && p_.n_macro_cols > 1 && L.n_assoc > 0) {
                const int M = p_.n_macro_cols;
                const int top_c = std::max(1, std::min(M, p_.trn_top_k));
                for (i32 j : L.nmda_list) {
                    const int mc = (int)(((int64_t)j * M) / L.n_assoc);
                    L.col_energy[(size_t)mc] += 0.35f * L.nmda_trace[(size_t)j];
                }
                std::vector<std::pair<float, int>> order_mc((size_t)M);
                float max_e = 0.0f;
                for (int mc = 0; mc < M; ++mc) {
                    L.col_energy[(size_t)mc] *= 0.72f;
                    L.col_habituation[(size_t)mc] *= 0.996f;
                    const float score = L.col_energy[(size_t)mc] - 0.18f * L.col_habituation[(size_t)mc];
                    order_mc[(size_t)mc] = {score, mc};
                    if (L.col_energy[(size_t)mc] > max_e) max_e = L.col_energy[(size_t)mc];
                }
                if (max_e > 0.05f) {
                    std::partial_sort(order_mc.begin(), order_mc.begin() + top_c, order_mc.end(),
                                      [](const auto& a, const auto& b) { return a.first > b.first; });
                    std::fill(L.col_gain.begin(), L.col_gain.end(), 0.34f);
                    for (int k = 0; k < top_c; ++k) {
                        const int win_mc = order_mc[(size_t)k].second;
                        L.col_gain[(size_t)win_mc] = 1.06f;
                        if (homeostasis_enabled_) L.col_habituation[(size_t)win_mc] += 0.015f;
                    }
                } else {
                    std::fill(L.col_gain.begin(), L.col_gain.end(), 1.0f);
                }
            }
        }
    }

    // Historial denso para tap_features() (solo si fue reservado o en modo 1 capa).
    if (!tap_hist_.empty() && (int)tap_hist_[0].size() == n_neurons_) {
        for (size_t k = tap_hist_.size() - 1; k > 0; --k)
            tap_hist_[k].swap(tap_hist_[k - 1]);
        int idx = 0;
        const float inv = 1.0f / std::max(1e-3f, p_.v_th);
        for (const auto& L : layers_)
            for (int j = 0; j < L.n; ++j) tap_hist_[0][(size_t)idx++] = L.v[(size_t)j] * inv;
    }

    // Historial disperso O(activos) de Capa 0 para cortical_sparse_features().
    if (p_.cortical_hierarchy) {
        if (l0_sparse_taps_.size() < 4) l0_sparse_taps_.assign(4, {});
        l0_tap_head_ = (l0_tap_head_ + 3) & 3;
        auto& cur_tap = l0_sparse_taps_[(size_t)l0_tap_head_];
        cur_tap.clear();
        const Layer& L0 = layers_[0];
        const float inv = 1.0f / std::max(1e-3f, p_.v_th);
        for (i32 j : L0.active_list) {
            const float val = L0.v[(size_t)j] * inv;
            if (val > 0.08f) cur_tap.push_back({j, val});
        }
    }

    if (p_.pace_lock && !pace_phase_.empty()) {
        const int np = (int)pace_phase_.size();
        for (int i = 0; i < np; ++i) {
            float f = p_.pace_phase_reset + (float)i / (float)np;
            if (f >= 1.0f) f -= 1.0f;
            pace_phase_[(size_t)i] = f;
        }
    }

    if (!p_.cortical_hierarchy) {
        if (p_.n_ctx > 1)
            std::memmove(&ctx_hist_[p_.n_chars], &ctx_hist_[0],
                         sizeof(float) * (size_t)p_.n_chars * (p_.n_ctx - 1));
        std::fill(ctx_hist_.begin(), ctx_hist_.begin() + p_.n_chars, 0.0f);
    }
    symbols_++;
}

void Engine::feed(int ch) {
    if (ch < 0 || ch >= p_.n_chars) throw std::out_of_range("simbolo fuera de rango");
    if (!p_.cortical_hierarchy) ctx_hist_[(size_t)ch] = 1.0f;
    ch_actual_ = ch;
    inject_only(ch);
    for (int s = 0; s < p_.ticks_per_char; ++s) {
        tick_in_sym_ = s;
        step();
    }
    tick_in_sym_ = -1;
    end_symbol();

    // Tercer factor dopaminergico: en jerarquia cortical solo corre si hay diccionario/bigramas.
    if (!p_.cortical_hierarchy || !dic_.empty() || !bigr_.empty()) {
        const float ev = juzgar_palabra(ch);
        const float caida = std::exp(-(float)p_.ticks_per_char / std::max(1.0f, p_.dop_tau));
        dop_ *= caida;
        if (ev > 0.0f) {
            dop_ += p_.dop_rise * ev;
            dop_pos_++;
        } else if (ev < 0.0f) {
            dop_ += p_.dop_rise * ev;
            dop_neg_++;
        }
        if (dop_ > 1.0f) dop_ = 1.0f;
        if (dop_ < -1.0f) dop_ = -1.0f;

        if (p_.dop_gain > 0.0f && dop_ != 0.0f) {
            const float delta = p_.dop_gain * 0.05f * dop_;
            Layer& L0 = layers_[0];
            if (delta > 0.0f) {
                for (int j = 0; j < L0.n; ++j) {
                    L0.v[(size_t)j] += delta;
                    if (!L0.active[(size_t)j]) {
                        L0.active[(size_t)j] = 1;
                        L0.active_list.push_back(j);
                    }
                }
            } else {
                // Con delta < 0, las neuronas en 0 seguirian clampeadas a 0 en el proximo tick:
                // basta actualizar las activas en O(activos) para resultado exacto.
                for (i32 j : L0.active_list) {
                    L0.v[(size_t)j] += delta;
                }
            }
        }
    }
}

void Engine::tap_hist_reserve(int n) {
    tap_hist_.assign((size_t)std::max(1, n), std::vector<float>((size_t)n_neurons_, 0.0f));
}

void Engine::ctx_features(std::vector<float>& out) const {
    const int F = p_.n_ctx_features();
    out.assign((size_t)F, 0.0f);
    for (int i = 0; i < F - 1 && i < (int)ctx_hist_.size(); ++i) out[(size_t)i] = ctx_hist_[(size_t)i];
    out[(size_t)F - 1] = 1.0f;
}

void Engine::rate_features(std::vector<float>& out) const {
    out.assign((size_t)n_neurons_, 0.0f);
    int idx = 0;
    for (const auto& L : layers_) {
        float nrm = 0.0f;
        for (int j = 0; j < L.n; ++j) nrm += L.rate[(size_t)j] * L.rate[(size_t)j];
        nrm = std::sqrt(nrm);
        const float g = nrm > 1e-6f ? 1.0f / nrm : 0.0f;
        for (int j = 0; j < L.n; ++j) out[(size_t)idx++] = L.rate[(size_t)j] * g;
    }
}

void Engine::last_features(std::vector<float>& out) const {
    out.assign((size_t)n_neurons_, 0.0f);
    int idx = 0;
    for (const auto& L : layers_) {
        float nrm = 0.0f;
        for (i32 j : L.last_spiked) nrm += L.rate_last[(size_t)j] * L.rate_last[(size_t)j];
        nrm = std::sqrt(nrm);
        const float g = nrm > 1e-6f ? 1.0f / nrm : 0.0f;
        for (i32 j : L.last_spiked) out[(size_t)(idx + j)] = L.rate_last[(size_t)j] * g;
        idx += L.n;
    }
}

void Engine::theta_features(std::vector<float>& out) const {
    out.assign((size_t)n_neurons_, 0.0f);
    int idx = 0;
    for (const auto& L : layers_) {
        float nrm = 0.0f;
        for (int j = 0; j < L.n; ++j) nrm += L.theta[(size_t)j] * L.theta[(size_t)j];
        nrm = std::sqrt(nrm);
        const float g = nrm > 1e-6f ? 1.0f / nrm : 0.0f;
        for (int j = 0; j < L.n; ++j) out[(size_t)idx++] = L.theta[(size_t)j] * g;
    }
}

void Engine::state_features(std::vector<float>& out) const {
    out.assign((size_t)n_neurons_, 0.0f);
    int idx = 0;
    const float inv = 1.0f / std::max(1e-3f, p_.v_th);
    for (const auto& L : layers_)
        for (int j = 0; j < L.n; ++j) out[(size_t)idx++] = L.v[(size_t)j] * inv;
}

void Engine::full_features(std::vector<float>& out) const {
    std::vector<float> a, b;
    state_features(a);
    theta_features(b);
    out.assign((size_t)2 * n_neurons_, 0.0f);
    for (int i = 0; i < n_neurons_; ++i) out[(size_t)i] = a[(size_t)i];
    for (int i = 0; i < n_neurons_; ++i) out[(size_t)(n_neurons_ + i)] = b[(size_t)i];
}

void Engine::tap_features(std::vector<float>& out, int n_tap) const {
    const int n = std::max(1, n_tap);
    if (tap_hist_.empty() || (int)tap_hist_.size() != n ||
        (int)tap_hist_[0].size() != n_neurons_) {
        tap_hist_.assign((size_t)n, std::vector<float>((size_t)n_neurons_, 0.0f));
    }
    out.assign((size_t)n * n_neurons_, 0.0f);
    for (int k = 0; k < n; ++k)
        for (int i = 0; i < n_neurons_; ++i)
            out[(size_t)k * n_neurons_ + i] = tap_hist_[(size_t)k][(size_t)i];
}

int Engine::cortical_feature_dim(int n_l0_taps) const {
    const int taps = std::max(1, std::min(4, n_l0_taps));
    const int N0 = layers_.empty() ? 0 : layers_[0].n;
    int dim = taps * N0 + N0;  // taps de estado L0 + fatiga L0
    for (size_t li = 1; li < layers_.size(); ++li) dim += layers_[li].n;
    for (size_t li = 2; li < layers_.size(); ++li) {
        if (layers_[li].n_assoc > 0) dim += layers_[li].n_assoc; // canales NMDA (li >= 2)
    }
    return dim;
}

int Engine::nmda_concept_dim() const {
    int dim = 0;
    for (size_t li = 2; li < layers_.size(); ++li) {
        if (layers_[li].n_assoc > 0) dim += layers_[li].n_assoc;
    }
    if (dim == 0 && layers_.size() >= 2) dim = layers_.back().n_assoc;
    return dim;
}

void Engine::extract_nmda_sdr(std::vector<std::pair<i32, float>>& sdr, int top_k) const {
    sdr.clear();
    i32 base = 0;
    for (size_t li = 2; li < layers_.size(); ++li) {
        const Layer& L = layers_[li];
        if (L.n_assoc <= 0) continue;
        const float region_boost = (li >= 3) ? 1.15f : 1.0f;
        for (i32 j : L.nmda_list) {
            sdr.push_back({base + j, L.nmda_trace[(size_t)j] * region_boost});
        }
        base += L.n_assoc;
    }
    if ((int)sdr.size() > top_k && top_k > 0) {
        std::nth_element(sdr.begin(), sdr.begin() + top_k, sdr.end(),
                         [](const auto& a, const auto& b) { return a.second > b.second; });
        sdr.resize((size_t)top_k);
    }
    float sq = 0.0f;
    for (const auto& kv : sdr) sq += kv.second * kv.second;
    if (sq > 1e-8f) {
        const float inv = 1.0f / std::sqrt(sq);
        for (auto& kv : sdr) kv.second *= inv;
    }
    std::sort(sdr.begin(), sdr.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
}

void Engine::accumulate_episodic_sdr(std::vector<float>& acc_buf) const {
    const int dim = nmda_concept_dim();
    if (dim <= 0) return;
    if ((int)acc_buf.size() < dim) acc_buf.assign((size_t)dim, 0.0f);
    i32 base = 0;
    for (size_t li = 2; li < layers_.size(); ++li) {
        const Layer& L = layers_[li];
        if (L.n_assoc <= 0) continue;
        // L2 (corteza lexica/entorrinal, invariante a habituacion TRN) actua como ancla
        // de raices y palabras clave; regiones superiores aportan contexto talamocortical:
        const float w_reg = (li == 2) ? 1.35f : 1.10f;
        for (i32 j : L.last_spiked) {
            if (j < L.n_assoc) {
                float& slot = acc_buf[(size_t)(base + j)];
                if (slot <= 0.0f) slot = w_reg;
                else slot = std::min(2.40f, slot + 0.18f * w_reg);
            }
        }
        base += L.n_assoc;
    }
}

void Engine::finalize_episodic_sdr(const std::vector<float>& acc_buf,
                                   std::vector<std::pair<i32, float>>& sdr,
                                   int max_k) const {
    sdr.clear();
    for (size_t i = 0; i < acc_buf.size(); ++i) {
        if (acc_buf[i] > 0.0f) sdr.push_back({(i32)i, acc_buf[i]});
    }
    if (max_k > 0 && (int)sdr.size() > max_k) {
        std::nth_element(sdr.begin(), sdr.begin() + max_k, sdr.end(),
                         [](const auto& a, const auto& b) { return a.second > b.second; });
        sdr.resize((size_t)max_k);
    }
    float sq = 0.0f;
    for (const auto& kv : sdr) sq += kv.second * kv.second;
    if (sq > 1e-8f) {
        const float inv = 1.0f / std::sqrt(sq);
        for (auto& kv : sdr) kv.second *= inv;
    }
    std::sort(sdr.begin(), sdr.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
}

void Engine::cortical_sparse_features(std::vector<std::pair<i32, float>>& out,
                                      int n_l0_taps,
                                      float nmda_gain) const {
    out.clear();
    if (layers_.empty()) return;
    const int taps = std::max(1, std::min(4, n_l0_taps));
    const Layer& L0 = layers_[0];
    const int N0 = L0.n;

    // 1. Taps dispersos del estado de Capa 0 (normalizados L2 por tap).
    if (!l0_sparse_taps_.empty()) {
        for (int k = 0; k < taps; ++k) {
            const auto& tp = l0_sparse_taps_[(size_t)((l0_tap_head_ + k) & 3)];
            float sq = 0.0f;
            for (const auto& kv : tp) sq += kv.second * kv.second;
            const float g = sq > 1e-8f ? 1.0f / std::sqrt(sq) : 0.0f;
            const i32 base = k * N0;
            for (const auto& kv : tp) out.push_back({base + kv.first, kv.second * g});
        }
    }

    // 2. Fatiga dispersa reciente de Capa 0 (solo theta > 0.08).
    {
        const i32 base = taps * N0;
        float sq = 0.0f;
        for (i32 j : L0.active_list) {
            const float th = L0.theta[(size_t)j];
            if (th > 0.08f) sq += th * th;
        }
        if (sq > 1e-8f) {
            const float g = 0.8f / std::sqrt(sq);
            for (i32 j : L0.active_list) {
                const float th = L0.theta[(size_t)j];
                if (th > 0.08f) out.push_back({base + j, th * g});
            }
        }
    }

    // 3. Regiones corticales superiores (li >= 1):
    //    - Asambleas de coincidencia que dispararon en este simbolo (`last_spiked`)
    //    - Neuronas de memoria lenta con potencial alto (`j >= n_assoc`)
    i32 base = (taps + 1) * N0;
    for (size_t li = 1; li < layers_.size(); ++li) {
        const Layer& L = layers_[li];
        if (!L.last_spiked.empty()) {
            const float ampa_g = (li == 1) ? 1.35f : ((li == 2) ? 1.25f : 0.78f);
            const float g = ampa_g / std::sqrt((float)L.last_spiked.size());
            for (i32 j : L.last_spiked) {
                out.push_back({base + j, L.rate_last[(size_t)j] * g});
            }
        }
        if (L.n_assoc < L.n) {
            const float thr_slow = 0.35f * p_.v_th;
            float sq = 0.0f;
            for (i32 j : L.active_list) {
                if (j >= L.n_assoc && L.v[(size_t)j] > thr_slow)
                    sq += L.v[(size_t)j] * L.v[(size_t)j];
            }
            if (sq > 1e-8f) {
                const float g = 0.45f / std::sqrt(sq);
                for (i32 j : L.active_list) {
                    if (j >= L.n_assoc && L.v[(size_t)j] > thr_slow)
                        out.push_back({base + j, L.v[(size_t)j] * g});
                }
            }
        }
        base += L.n;
    }

    // 4. Canales sinapticos lentos tipo NMDA en todas las regiones altas (li >= 2):
    //    L2 aporta memoria lexica de frase y L3+ aporta memoria relacional/prefrontal,
    //    cada una normalizada L2 en sus propios indices sin interferir con AMPA.
    if (layers_.size() >= 3 && nmda_gain > 0.0f) {
        const int n_nmda_regions = (int)layers_.size() - 2;
        const float reg_scale = 1.0f / std::sqrt((float)n_nmda_regions);
        for (size_t li = 2; li < layers_.size(); ++li) {
            const Layer& L = layers_[li];
            if (L.n_assoc <= 0) continue;
            if (!L.nmda_list.empty()) {
                float sq = 0.0f;
                for (i32 j : L.nmda_list) sq += L.nmda_trace[(size_t)j] * L.nmda_trace[(size_t)j];
                if (sq > 1e-8f) {
                    const float g = (nmda_gain * reg_scale) / std::sqrt(sq);
                    for (i32 j : L.nmda_list) {
                        out.push_back({base + j, L.nmda_trace[(size_t)j] * g});
                    }
                }
            }
            base += L.n_assoc;
        }
    }
}

void Engine::ensure_readout_allocated() const {
    if (!R_.empty()) return;
    const int F = p_.n_features_max();
    R_.assign((size_t)p_.n_chars * F, 0.0f);
}

void Engine::normalize(const std::vector<float>& in, std::vector<float>& out) const {
    const int F = (int)in.size();
    out.assign((size_t)F, 0.0f);
    if (mu_.size() != (size_t)F) return;
    for (int i = 0; i < F; ++i) {
        const float sd = sd_[(size_t)i];
        out[(size_t)i] = sd > 1e-6f ? (in[(size_t)i] - mu_[(size_t)i]) / sd : 0.0f;
    }
}

void Engine::update_norm(const std::vector<float>& f) {
    const int F = (int)f.size();
    if (mu_.size() != (size_t)F) { mu_.assign((size_t)F, 0.0f); sd_.assign((size_t)F, 1.0f); return; }
    const float a = 0.001f;
    for (int i = 0; i < F; ++i) {
        const float d = f[(size_t)i] - mu_[(size_t)i];
        mu_[(size_t)i] += a * d;
        sd_[(size_t)i] = std::sqrt((1 - a) * (sd_[(size_t)i] * sd_[(size_t)i] + a * d * d));
        if (sd_[(size_t)i] < 1.0f) sd_[(size_t)i] = 1.0f;
    }
}

void Engine::readout(const std::vector<float>& f, float* logits) const {
    ensure_readout_allocated();
    const int F = (int)f.size();
    const int stride = p_.n_features_max();
    for (int k = 0; k < p_.n_chars; ++k) {
        float acc = 0.0f;
        const float* row = &R_[(size_t)k * stride];
        for (int i = 0; i < F; ++i) acc += row[i] * f[(size_t)i];
        logits[k] = acc + (logits_b_.empty() ? 0.0f : logits_b_[(size_t)k]);
    }
}

void Engine::error_local(int capa, std::vector<float>& out) {
    if (capa < 0 || capa >= (int)layers_.size()) { out.clear(); return; }
    Layer& L = layers_[(size_t)capa];
    if (L.pred.empty()) L.pred.assign((size_t)L.n, 0.0f);
    out.assign((size_t)L.n, 0.0f);

    std::vector<float> objetivo((size_t)L.n, 0.0f);
    if (capa == 0 && ch_actual_ >= 0 && ch_actual_ < p_.n_chars) {
        const int patch = p_.patch_size();
        const int tex = std::max(4, patch / 2);
        const int per = patch + tex;
        const int* dst = &sensory_[(size_t)ch_actual_ * per];
        for (int i = 0; i < per; ++i) objetivo[(size_t)dst[i]] = 1.0f;
    }

    float maxr = 0.0f;
    for (int j = 0; j < L.n; ++j) maxr = std::max(maxr, L.rate[(size_t)j]);
    const float inv = maxr > 1e-6f ? 1.0f / maxr : 0.0f;

    for (int j = 0; j < L.n; ++j) {
        const float real = L.rate[(size_t)j] * inv;
        L.pred[(size_t)j] += p_.pred_lr * (objetivo[(size_t)j] - L.pred[(size_t)j]);
        out[(size_t)j] = p_.pred_gain * (real - L.pred[(size_t)j]);
    }
    L.pred_err = out;
}

void Engine::stats_error_local(int capa, float& medio, float& maximo) const {
    medio = 0.0f;
    maximo = 0.0f;
    if (capa < 0 || capa >= (int)layers_.size()) return;
    const Layer& L = layers_[(size_t)capa];
    if (L.pred_err.empty()) return;
    double acc = 0.0;
    for (float e : L.pred_err) {
        acc += std::fabs(e);
        maximo = std::max(maximo, std::fabs(e));
    }
    medio = (float)(acc / (double)L.pred_err.size());
}

void Engine::cargar_diccionario(const std::string& ruta) {
    std::ifstream in(ruta);
    if (!in) throw std::runtime_error("no se puede abrir el diccionario: " + ruta);
    std::string w;
    dic_.clear();
    p_.diccionario.clear();
    while (std::getline(in, w)) {
        while (!w.empty() && (w.back() == '\r' || w.back() == ' ')) w.pop_back();
        if (w.size() < 2) continue;
        for (auto& c : w) c = (char)std::tolower((unsigned char)c);
        dic_.insert(w);
        p_.diccionario.push_back(w);
    }
}

void Engine::cargar_bigramas(const std::string& ruta) {
    std::ifstream in(ruta, std::ios::binary);
    if (!in) throw std::runtime_error("no se puede abrir el corpus: " + ruta);
    std::unordered_map<uint32_t, int> cnt;
    int prev = -1;
    char c;
    while (in.get(c)) {
        const uint32_t a = (uint32_t)(unsigned char)c;
        if (prev >= 0) cnt[((uint32_t)prev << 8) | a]++;
        prev = (int)a;
    }
    bigr_.clear();
    bigr_.reserve(cnt.size() * 2);
    int total = 0;
    for (auto& kv : cnt) total += kv.second;
    for (auto& kv : cnt) bigr_[kv.first] = (float)kv.second / (float)std::max(1, total);
    bigr_umbral_ = 1.0f / (float)std::max(1000, total);
}

float Engine::juzgar_palabra(int ultimo) {
    dop_evento_ = 0.0f;
    const bool es_letra = (ultimo >= 0) && !std::isspace(ultimo) &&
                          ultimo != '.' && ultimo != ',' && ultimo != ';' &&
                          ultimo != ':' && ultimo != '!' && ultimo != '?' &&
                          ultimo != '"' && ultimo != '(' && ultimo != ')';

    if (es_letra) {
        palabra_.push_back((char)std::tolower(ultimo));
        if (!bigr_.empty() && palabra_.size() >= 2 && dop_evento_ == 0.0f) {
            const uint32_t k = ((uint32_t)(unsigned char)palabra_[palabra_.size() - 2] << 8)
                             | (uint32_t)(unsigned char)palabra_.back();
            auto it = bigr_.find(k);
            const float p = (it == bigr_.end()) ? 0.0f : it->second;
            if (p < bigr_umbral_) dop_evento_ = -0.5f;
        }
        return dop_evento_;
    }

    if ((int)palabra_.size() >= p_.dop_min_len) {
        if (!dic_.empty() && dic_.count(palabra_) > 0)
            dop_evento_ = +1.0f;
        else
            dop_evento_ = -1.0f;
    }
    palabra_.clear();
    return dop_evento_;
}

void Engine::learn(const float* error, const std::vector<float>& f) {
    ensure_readout_allocated();
    if (R_m_.empty()) {
        R_m_.assign(R_.size(), 0.0f);
        R_v_.assign(R_.size(), 0.0f);
        logits_b_.assign((size_t)p_.n_chars, 0.0f);
        feedback_.assign((size_t)n_neurons_, 0.0f);
        fb_prev_.assign((size_t)n_neurons_, 0.0f);
    }
    if (error != nullptr && !R_.empty()) {
        std::vector<float> fn;
        normalize(f, fn);
        std::fill(feedback_.begin(), feedback_.end(), 0.0f);
        const int stride = p_.n_features_max();
        const int F = std::min((int)fn.size(), stride);
        for (int j = 0; j < F && j < (int)feedback_.size(); ++j) {
            float acc = 0.0f;
            for (int k = 0; k < p_.n_chars; ++k) acc += R_[(size_t)k * stride + j] * error[k];
            feedback_[(size_t)j] = p_.eprop_scale * acc;
        }
        for (int j = 0; j < n_neurons_; ++j)
            feedback_[(size_t)j] = p_.eta_slow * fb_prev_[(size_t)j] + (1.0f - p_.eta_slow) * feedback_[(size_t)j];
        fb_prev_ = feedback_;
    }

    std::vector<float> s_local;
    for (size_t li = 0; li < layers_.size(); ++li) {
        Layer& L = layers_[li];
        if (L.pre_trace.empty() || L.g.w_init.empty()) continue;
        const int n = L.n;
        error_local((int)li, s_local);
        const bool usar_local = !s_local.empty();
        const float decay_post_w = p_.trace_post_w;

        for (int pre = 0; pre < n; ++pre) {
            const float tr = L.pre_trace[(size_t)pre];
            if (tr < 1e-4f) continue;
            const i32 s0 = L.g.offsets[(size_t)pre], s1 = L.g.offsets[(size_t)pre + 1];
            for (i32 s = s0; s < s1; ++s) {
                const i32 post = L.g.post[(size_t)s];
                const float tp = decay_post_w > 0.0f
                               ? (1.0f - decay_post_w) + decay_post_w * L.post_trace[(size_t)post]
                               : 1.0f;
                if (tp < 1e-4f) continue;
                float aligned = tr;
                for (int d = 0; d < (int)L.g.delay[(size_t)s]; ++d) aligned *= L.alpha_elig;
                const float sig = usar_local ? s_local[(size_t)post]
                                             : feedback_[(size_t)(L.offset + post)];
                const float mod = 1.0f + p_.dop_gain * dop_;
                const float dw = -p_.eta * mod * sig * aligned * tp;
                float nw = L.g.weight[(size_t)s] + dw;
                const float w0 = L.g.w_init[(size_t)s];
                const float lo = std::max(p_.w_min, w0 * (1.0f - p_.w_frac_max));
                const float hi = std::min(p_.w_max, w0 * (1.0f + p_.w_frac_max));
                L.g.weight[(size_t)s] = std::max(lo, std::min(hi, nw));
            }
        }
    }

    if (error != nullptr) {
        std::vector<float> fn;
        normalize(f, fn);
        const int F = (int)fn.size();
        const int stride = p_.n_features_max();
        const float lr = p_.readout_lr;
        const float b1 = 0.9f, b2 = 0.999f, eps = 1e-5f;
        for (int k = 0; k < p_.n_chars; ++k) {
            float g = std::max(-p_.gclip, std::min(p_.gclip, error[k]));
            logits_b_[(size_t)k] -= lr * 0.25f * g;
            logits_b_[(size_t)k] = std::max(-4.0f, std::min(4.0f, logits_b_[(size_t)k]));
            float* row = &R_[(size_t)k * stride];
            float* m   = &R_m_[(size_t)k * stride];
            float* v   = &R_v_[(size_t)k * stride];
            for (int i = 0; i < F; ++i) {
                const float gr = g * fn[(size_t)i];
                m[i] = b1 * m[i] + (1 - b1) * gr;
                v[i] = b2 * v[i] + (1 - b2) * gr * gr;
                row[i] -= lr * m[i] / (std::sqrt(v[i]) + eps);
                row[i] = std::max(-8.0f, std::min(8.0f, row[i]));
            }
        }
    }
}

int Engine::max_cpu_threads() {
#ifdef _OPENMP
    return std::max(1, omp_get_max_threads());
#else
    return 1;
#endif
}

std::vector<std::pair<int, float>> Engine::active_macro_columns(int layer_idx) const {
    std::vector<std::pair<int, float>> res;
    if (layers_.empty() || p_.n_macro_cols <= 1) return res;
    int li = layer_idx;
    if (li < 0 || li >= (int)layers_.size()) li = (int)layers_.size() - 1;
    const Layer& L = layers_[(size_t)li];
    if (L.col_energy.empty()) return res;
    for (int mc = 0; mc < (int)L.col_energy.size(); ++mc) {
        if (L.col_energy[(size_t)mc] > 0.01f) {
            res.push_back({mc, L.col_energy[(size_t)mc]});
        }
    }
    std::sort(res.begin(), res.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if ((int)res.size() > std::max(1, p_.trn_top_k)) res.resize((size_t)std::max(1, p_.trn_top_k));
    return res;
}

// ===========================================================================
// PERSISTENCIA BINARIA DEL MOTOR
// ===========================================================================
void Engine::save_engine(std::ostream& os) const {
    const bool has_trn = (p_.n_macro_cols > 1);
    const uint32_t magic = has_trn ? 0x43584533u : 0x43584532u; // CXE3 si TRN activo, CXE2 compatible
    write_pod(os, magic);
    write_pod(os, p_.n_layers);
    write_pod(os, p_.layer_size);
    write_vec(os, p_.layer_sizes);
    write_pod(os, p_.max_out_degree);
    write_pod(os, p_.n_chars);
    write_pod(os, p_.density);
    write_pod(os, p_.d_max);
    write_pod(os, p_.dt);
    write_pod(os, p_.tau_m);
    write_pod(os, p_.v_th);
    write_pod(os, p_.v_reset);
    write_pod(os, p_.refractory);
    write_pod(os, p_.ticks_per_char);
    write_pod(os, p_.input_gain);
    write_pod(os, p_.input_spread);
    write_pod(os, p_.input_patch);
    write_pod(os, p_.w_scale);
    write_pod(os, p_.active_frac);
    write_pod(os, p_.inh_gain);
    write_pod(os, p_.inh_tau);
    write_pod(os, p_.theta_step);
    write_pod(os, p_.theta_decay);
    write_pod(os, p_.theta_max);
    write_pod(os, p_.cortical_hierarchy);
    write_pod(os, p_.assoc_tau_m);
    write_pod(os, p_.assoc_theta_step);
    write_pod(os, p_.assoc_theta_decay);
    write_pod(os, p_.assoc_fan_in_slot);
    write_pod(os, p_.assoc_k_wta);
    write_pod(os, p_.stdp_ltp);
    write_pod(os, p_.stdp_ltd);
    write_pod(os, p_.homeo_thr_up);
    write_pod(os, p_.homeo_thr_down);
    write_pod(os, p_.homeo_thr_max);
    write_pod(os, p_.slow_ctx_frac);
    write_pod(os, p_.slow_ctx_tau_m);
    write_pod(os, p_.slow_ctx_gain);
    write_pod(os, p_.seed);
    if (has_trn) {
        write_pod(os, p_.n_macro_cols);
        write_pod(os, p_.trn_top_k);
    }

    const uint32_t nl = (uint32_t)layers_.size();
    write_pod(os, nl);
    for (const auto& L : layers_) {
        write_pod(os, L.n);
        write_pod(os, L.offset);
        write_pod(os, L.n_assoc);
        write_pod(os, L.tau_m_local);
        write_pod(os, L.theta_step_local);
        write_pod(os, L.theta_decay_local);
        write_pod(os, L.inh_gain_local);
        write_pod(os, L.k_wta_local);
        write_vec(os, L.bias);
        write_vec(os, L.homeo_thr);
        write_vec(os, L.dend_order);
        write_vec(os, L.g.offsets);
        write_vec(os, L.g.post);
        write_vec(os, L.g.weight);
        write_vec(os, L.g.delay);
        if (has_trn) write_vec(os, L.col_habituation);
    }

    const uint32_t ni = (uint32_t)inter_.size();
    write_pod(os, ni);
    for (const auto& ig : inter_) {
        write_pod(os, ig.src_layer);
        write_pod(os, ig.dst_layer);
        write_vec(os, ig.offsets);
        write_vec(os, ig.post_slot);
        write_vec(os, ig.weight);
        write_vec(os, ig.delay);
        write_vec(os, ig.in_offsets);
        write_vec(os, ig.in_syn);
        write_vec(os, ig.in_pre);
    }
}

void Engine::load_engine(std::istream& is) {
    uint32_t magic = 0;
    read_pod(is, magic);
    if (magic != 0x43584533u && magic != 0x43584532u && magic != 0x43584531u) {
        throw std::runtime_error("formato de motor invalido");
    }
    const bool legacy_cxe1 = (magic == 0x43584531u);
    const bool is_cxe3     = (magic == 0x43584533u);

    read_pod(is, p_.n_layers);
    read_pod(is, p_.layer_size);
    read_vec(is, p_.layer_sizes);
    read_pod(is, p_.max_out_degree);
    read_pod(is, p_.n_chars);
    read_pod(is, p_.density);
    read_pod(is, p_.d_max);
    read_pod(is, p_.dt);
    read_pod(is, p_.tau_m);
    read_pod(is, p_.v_th);
    read_pod(is, p_.v_reset);
    read_pod(is, p_.refractory);
    read_pod(is, p_.ticks_per_char);
    read_pod(is, p_.input_gain);
    read_pod(is, p_.input_spread);
    read_pod(is, p_.input_patch);
    read_pod(is, p_.w_scale);
    read_pod(is, p_.active_frac);
    read_pod(is, p_.inh_gain);
    read_pod(is, p_.inh_tau);
    read_pod(is, p_.theta_step);
    read_pod(is, p_.theta_decay);
    read_pod(is, p_.theta_max);
    read_pod(is, p_.cortical_hierarchy);
    read_pod(is, p_.assoc_tau_m);
    read_pod(is, p_.assoc_theta_step);
    read_pod(is, p_.assoc_theta_decay);
    read_pod(is, p_.assoc_fan_in_slot);
    read_pod(is, p_.assoc_k_wta);
    read_pod(is, p_.stdp_ltp);
    read_pod(is, p_.stdp_ltd);
    read_pod(is, p_.homeo_thr_up);
    read_pod(is, p_.homeo_thr_down);
    read_pod(is, p_.homeo_thr_max);
    read_pod(is, p_.slow_ctx_frac);
    read_pod(is, p_.slow_ctx_tau_m);
    read_pod(is, p_.slow_ctx_gain);
    read_pod(is, p_.seed);
    if (is_cxe3) {
        read_pod(is, p_.n_macro_cols);
        read_pod(is, p_.trn_top_k);
    } else {
        p_.n_macro_cols = 0;
        p_.trn_top_k = 4;
    }

    build_sensory_map();
    ctx_hist_.assign((size_t)p_.n_ctx * p_.n_chars, 0.0f);

    uint32_t nl = 0;
    read_pod(is, nl);
    layers_.clear();
    layers_.resize(nl);
    queues_.clear();
    queues_.resize(nl);
    inh_prev_layer_.assign(nl, 0.0f);
    n_neurons_ = 0;

    for (uint32_t li = 0; li < nl; ++li) {
        Layer& L = layers_[li];
        read_pod(is, L.n);
        read_pod(is, L.offset);
        read_pod(is, L.n_assoc);
        read_pod(is, L.tau_m_local);
        read_pod(is, L.theta_step_local);
        read_pod(is, L.theta_decay_local);
        read_pod(is, L.inh_gain_local);
        read_pod(is, L.k_wta_local);
        read_vec(is, L.bias);
        read_vec(is, L.homeo_thr);
        read_vec(is, L.dend_order);
        read_vec(is, L.g.offsets);
        read_vec(is, L.g.post);
        read_vec(is, L.g.weight);
        read_vec(is, L.g.delay);
        if (is_cxe3) read_vec(is, L.col_habituation);
        else L.col_habituation.clear();
        L.g.n_syn = L.g.post.size();
        n_neurons_ += L.n;

        L.v.assign((size_t)L.n, p_.v_reset);
        L.theta.assign((size_t)L.n, 0.0f);
        L.rate.assign((size_t)L.n, 0.0f);
        L.rate_last.assign((size_t)L.n, 0.0f);
        L.refr.assign((size_t)L.n, 0);
        L.spike_count.assign((size_t)L.n, 0);
        L.active.assign((size_t)L.n, 0);
        L.sym_spiked_flag.assign((size_t)L.n, 0);
        L.last_spk.assign((size_t)L.n * 4, -100000);
        L.last_spk_wp.assign((size_t)L.n, 0);
        if (L.n_assoc > 0) {
            L.dend_v.assign((size_t)L.n_assoc * 5, 0.0f);
            L.dend_touched.assign((size_t)L.n_assoc, 0);
            L.pending_ticks = 0;
            if (li >= 2) {
                L.nmda_trace.assign((size_t)L.n_assoc, 0.0f);
                L.nmda_active.assign((size_t)L.n_assoc, 0);
                L.nmda_last_sym.assign((size_t)L.n_assoc, -10000);
                if (p_.n_macro_cols > 1) {
                    const size_t mc = (size_t)p_.n_macro_cols;
                    L.col_energy.assign(mc, 0.0f);
                    L.col_gain.assign(mc, 1.0f);
                    if (L.col_habituation.size() != mc) L.col_habituation.assign(mc, 0.0f);
                }
            }
        }
        int max_d = p_.d_max;
        for (uint8_t d : L.g.delay) if ((int)d > max_d) max_d = (int)d;
        queues_[li].init(max_d + 8);
    }

    uint32_t ni = 0;
    read_pod(is, ni);
    inter_.clear();
    inter_.resize(ni);
    out_inter_by_layer_.assign(nl, {});
    in_inter_by_layer_.assign(nl, {});
    for (uint32_t i = 0; i < ni; ++i) {
        InterGraph& ig = inter_[i];
        read_pod(is, ig.src_layer);
        read_pod(is, ig.dst_layer);
        read_vec(is, ig.offsets);
        read_vec(is, ig.post_slot);
        read_vec(is, ig.weight);
        read_vec(is, ig.delay);
        read_vec(is, ig.in_offsets);
        read_vec(is, ig.in_syn);
        read_vec(is, ig.in_pre);
        ig.n_syn = ig.post_slot.size();
        if (legacy_cxe1) {
            for (i32& ps : ig.post_slot) ps = ((ps >> 2) << 3) | (ps & 3);
        }
        out_inter_by_layer_[(size_t)ig.src_layer].push_back((int)i);
        in_inter_by_layer_[(size_t)ig.dst_layer].push_back((int)i);
        int max_d = 8;
        for (uint8_t d : ig.delay) if ((int)d > max_d) max_d = (int)d;
        if (max_d + 8 > queues_[(size_t)ig.dst_layer].span) {
            queues_[(size_t)ig.dst_layer].init(max_d + 8);
        }
    }

    reset_state();
}

// ===========================================================================
// CORTICAL READOUT DISPERSO O(activos * 256) + CONSOLIDACION CONTINUA
// ===========================================================================
void CorticalReadout::init(int n_features, float lr, float l2_reg) {
    F_ = n_features;
    n_active_ = 0;
    lr_ = lr;
    l2_reg_ = l2_reg;
    consol_pull_ = 0.0f;
    cal_steps_ = 0;
    calibrated_ = false;

    byte_to_slot_.assign((size_t)kVocab, -1);
    slot_to_byte_.clear();
    slot_to_byte_.reserve((size_t)kVocab);

    W_.assign((size_t)F_ * kVocab, 0.0f);
    W_anchor_.clear();
    imp_.assign((size_t)F_, 0.0f);
    scale_.assign((size_t)F_, 1.0f);
    sq_sum_.assign((size_t)F_, 0.0);
    b_.assign((size_t)kVocab, 0.0f);
    logits_.assign((size_t)kVocab, 0.0f);
    probs_.assign((size_t)kVocab, 0.0);
    err_.assign((size_t)kVocab, 0.0f);
}

int CorticalReadout::ensure_byte_slot(int byte_val) {
    const int y = byte_val & 255;
    int s = byte_to_slot_[(size_t)y];
    if (s < 0) {
        s = n_active_++;
        byte_to_slot_[(size_t)y] = s;
        slot_to_byte_.push_back(y);
    }
    return s;
}

void CorticalReadout::calibrate_step(const std::vector<std::pair<i32, float>>& feats) {
    cal_steps_++;
    for (const auto& kv : feats) {
        if (kv.first >= 0 && kv.first < F_) {
            sq_sum_[(size_t)kv.first] += (double)kv.second * (double)kv.second;
        }
    }
}

void CorticalReadout::freeze_calibration() {
    if (cal_steps_ <= 0) { calibrated_ = true; return; }
    const double inv_n = 1.0 / (double)cal_steps_;
    for (int i = 0; i < F_; ++i) {
        const double rms = std::sqrt(sq_sum_[(size_t)i] * inv_n);
        if (rms > 1e-5) {
            const double s = 0.22 / std::pow(rms, 0.30);
            scale_[(size_t)i] = (float)std::max(0.45, std::min(2.6, s));
        } else {
            scale_[(size_t)i] = 1.0f;
        }
    }
    calibrated_ = true;
}

void CorticalReadout::predict(const std::vector<std::pair<i32, float>>& feats,
                              double* probs_out,
                              double temperature) const {
    for (int c = 0; c < kVocab; ++c) probs_out[c] = 0.0;
    if (n_active_ <= 0) {
        for (int c = 0; c < kVocab; ++c) probs_out[c] = 1.0 / (double)kVocab;
        return;
    }

    scaled_feats_.clear();
    float norm_sq = 0.0f;
    for (const auto& kv : feats) {
        const i32 idx = kv.first;
        if (idx < 0 || idx >= F_) continue;
        const float z = kv.second * (calibrated_ ? scale_[(size_t)idx] : 1.0f);
        if (std::fabs(z) < 1e-7f) continue;
        scaled_feats_.push_back({idx, z});
        norm_sq += z * z;
    }
    const float vec_scale = norm_sq > 1e-6f ? 1.0f / std::sqrt(norm_sq) : 1.0f;
    if (norm_sq > 1e-6f) {
        for (auto& kv : scaled_feats_) kv.second *= vec_scale;
    }

    const int C = n_active_;
    const int C8 = (C + 7) & ~7;
    float* __restrict__ lg = logits_.data();
    for (int s = 0; s < C; ++s) lg[s] = b_[(size_t)s];
    for (int s = C; s < C8; ++s) lg[s] = 0.0f;

    for (const auto& kv : scaled_feats_) {
        const float z = kv.second;
        const float* __restrict__ w = &W_[(size_t)kv.first * kVocab];
        for (int s = 0; s < C8; ++s) lg[s] += z * w[s];
    }

    const double inv_t = 1.0 / std::max(0.05, temperature);
    double mx = -1e30;
    for (int s = 0; s < C; ++s) {
        const double v = (double)lg[s] * inv_t;
        probs_[(size_t)s] = v;
        if (v > mx) mx = v;
    }
    double sm = 0.0;
    for (int s = 0; s < C; ++s) {
        probs_[(size_t)s] = std::exp(probs_[(size_t)s] - mx);
        sm += probs_[(size_t)s];
    }
    const double inv_sm = 1.0 / std::max(1e-30, sm);
    for (int s = 0; s < C; ++s) {
        probs_out[slot_to_byte_[(size_t)s]] = probs_[(size_t)s] * inv_sm;
    }
}

double CorticalReadout::step(int target_byte,
                             const std::vector<std::pair<i32, float>>& feats,
                             bool learn,
                             float mod) {
    const int y = target_byte & 255;
    int target_slot = byte_to_slot_[(size_t)y];
    if (target_slot < 0 && learn) {
        target_slot = ensure_byte_slot(y);
    }
    const int C = n_active_;
    if (C <= 0) return 8.0; // log2(256) cuando aun no se ha visto ningun byte

    scaled_feats_.clear();
    float norm_sq = 0.0f;
    for (const auto& kv : feats) {
        const i32 idx = kv.first;
        if (idx < 0 || idx >= F_) continue;
        const float z = kv.second * (calibrated_ ? scale_[(size_t)idx] : 1.0f);
        if (std::fabs(z) < 1e-7f) continue;
        scaled_feats_.push_back({idx, z});
        norm_sq += z * z;
    }
    const float vec_scale = norm_sq > 1e-6f ? 1.0f / std::sqrt(norm_sq) : 1.0f;
    if (norm_sq > 1e-6f) {
        for (auto& kv : scaled_feats_) kv.second *= vec_scale;
    }

    const int C8 = (C + 7) & ~7;
    float* __restrict__ lg = logits_.data();
    for (int s = 0; s < C; ++s) lg[s] = b_[(size_t)s];
    for (int s = C; s < C8; ++s) lg[s] = 0.0f;
    for (const auto& kv : scaled_feats_) {
        const float z = kv.second;
        const float* __restrict__ w = &W_[(size_t)kv.first * kVocab];
        for (int s = 0; s < C8; ++s) lg[s] += z * w[s];
    }

    double mx = -1e30;
    for (int s = 0; s < C; ++s) if ((double)lg[s] > mx) mx = (double)lg[s];
    double sm = 0.0;
    for (int s = 0; s < C; ++s) {
        probs_[(size_t)s] = std::exp((double)lg[s] - mx);
        sm += probs_[(size_t)s];
    }
    const double inv_sm = 1.0 / sm;
    for (int s = 0; s < C; ++s) probs_[(size_t)s] *= inv_sm;

    // Si en holdout aparece un byte jamas visto en entrenamiento, le damos piso
    // honesto 1e-6 sobre 256 bytes (sin trampa ni caida a -inf).
    const double py = (target_slot >= 0) ? std::max(1e-9, probs_[(size_t)target_slot]) : 1e-6;

    if (learn && mod != 0.0f && target_slot >= 0) {
        const float step_lr = lr_ * mod;
        float* __restrict__ err = err_.data();
        for (int s = 0; s < C; ++s) {
            const double g = probs_[(size_t)s] - (s == target_slot ? 1.0 : 0.0);
            err[s] = (float)(step_lr * g);
            b_[(size_t)s] -= (float)(0.15 * (double)step_lr * g);
        }
        for (int s = C; s < C8; ++s) err[s] = 0.0f;

        const bool has_anchor = (!W_anchor_.empty() && consol_pull_ > 0.0f);
        for (const auto& kv : scaled_feats_) {
            const size_t fi = (size_t)kv.first;
            const float z = kv.second;
            imp_[fi] = std::min(1.0f, imp_[fi] + 0.0002f * std::fabs(z));
            float* __restrict__ w = &W_[fi * kVocab];
            if (!has_anchor) {
                if (l2_reg_ > 0.0f) {
                    const float dec = 1.0f - step_lr * l2_reg_;
                    for (int s = 0; s < C8; ++s) w[s] = w[s] * dec - z * err[s];
                } else {
                    for (int s = 0; s < C8; ++s) w[s] -= z * err[s];
                }
            } else {
                const float* __restrict__ wa = &W_anchor_[fi * kVocab];
                const float pull = step_lr * consol_pull_ * imp_[fi];
                const float keep = 1.0f - pull;
                for (int s = 0; s < C8; ++s) {
                    w[s] = keep * w[s] + pull * wa[s] - z * err[s];
                }
            }
        }
    }

    return -std::log2(py);
}

void CorticalReadout::scale_weights(float factor) {
    for (auto& x : W_) x *= factor;
    for (auto& x : b_) x *= factor;
    W_anchor_.clear();
    consol_pull_ = 0.0f;
}

void CorticalReadout::consolidate(float strength) {
    if (W_anchor_.size() == W_.size()) {
        for (size_t i = 0; i < W_.size(); ++i) {
            W_anchor_[i] = 0.65f * W_[i] + 0.35f * W_anchor_[i];
        }
    } else {
        W_anchor_ = W_;
    }
    consol_pull_ = strength;
}

void CorticalReadout::save(std::ostream& os) const {
    const uint32_t magic = 0x43585232u; // "CXR2"
    write_pod(os, magic);
    write_pod(os, F_);
    write_pod(os, n_active_);
    write_pod(os, lr_);
    write_pod(os, l2_reg_);
    write_pod(os, consol_pull_);
    write_pod(os, cal_steps_);
    write_pod(os, calibrated_);
    write_vec(os, byte_to_slot_);
    write_vec(os, slot_to_byte_);
    write_vec(os, W_);
    write_vec(os, W_anchor_);
    write_vec(os, imp_);
    write_vec(os, scale_);
    write_vec(os, sq_sum_);
    write_vec(os, b_);
}

void CorticalReadout::load(std::istream& is) {
    uint32_t magic = 0;
    read_pod(is, magic);
    if (magic != 0x43585232u) throw std::runtime_error("formato de lector invalido");
    read_pod(is, F_);
    read_pod(is, n_active_);
    read_pod(is, lr_);
    read_pod(is, l2_reg_);
    read_pod(is, consol_pull_);
    read_pod(is, cal_steps_);
    read_pod(is, calibrated_);
    read_vec(is, byte_to_slot_);
    read_vec(is, slot_to_byte_);
    read_vec(is, W_);
    read_vec(is, W_anchor_);
    read_vec(is, imp_);
    read_vec(is, scale_);
    read_vec(is, sq_sum_);
    read_vec(is, b_);
    logits_.assign((size_t)kVocab, 0.0f);
    probs_.assign((size_t)kVocab, 0.0);
    err_.assign((size_t)kVocab, 0.0f);
}

void save_brain(const std::string& path, const Engine& eng, const CorticalReadout& rd) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("no se puede escribir el modelo en: " + path);
    const uint32_t magic = 0x4E554259u; // "NUBY" / CORTEX brain
    write_pod(out, magic);
    eng.save_engine(out);
    rd.save(out);
    if (!out) throw std::runtime_error("error al guardar el modelo en disco: " + path);
}

void load_brain(const std::string& path, Engine& eng, CorticalReadout& rd) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("no se puede abrir el modelo: " + path);
    uint32_t magic = 0;
    read_pod(in, magic);
    if (magic != 0x4E554259u) throw std::runtime_error("archivo no es un cerebro CORTEX-LM valido");
    eng.load_engine(in);
    rd.load(in);
}

// ===========================================================================
// SUBREGION HIPOCAMPAL CA3 (MEMORIA EPISODICA SINAPTICA EN 1 DISPARO)
// ===========================================================================
void EpisodicCA3::init(int n_concept_dims) {
    dim_ = std::max(0, n_concept_dims);
    engrams_.clear();
    syn_from_concept_.assign((size_t)dim_, {});
    exc_buf_.clear();
    touched_engrams_.clear();
}

int EpisodicCA3::memorize(const std::vector<std::pair<i32, float>>& sdr, const std::string& trace) {
    if (sdr.empty() || dim_ <= 0) return -1;
    const i32 eid = (i32)engrams_.size();
    engrams_.push_back(trace);
    exc_buf_.resize(engrams_.size(), 0.0f);
    for (const auto& kv : sdr) {
        if (kv.first >= 0 && kv.first < dim_ && kv.second > 0.0f) {
            syn_from_concept_[(size_t)kv.first].push_back({eid, kv.second});
        }
    }
    return (int)eid;
}

bool EpisodicCA3::recall(const std::vector<std::pair<i32, float>>& query_sdr,
                         std::string& out_trace,
                         float min_sim,
                         float* out_score) const {
    if (engrams_.empty() || query_sdr.empty() || dim_ <= 0) {
        if (out_score) *out_score = 0.0f;
        return false;
    }
    if (exc_buf_.size() < engrams_.size()) exc_buf_.resize(engrams_.size(), 0.0f);
    touched_engrams_.clear();

    for (const auto& kv : query_sdr) {
        const i32 c = kv.first;
        const float qv = kv.second;
        if (c < 0 || c >= dim_ || qv <= 0.0f) continue;
        for (const auto& syn : syn_from_concept_[(size_t)c]) {
            const i32 eid = syn.first;
            if (exc_buf_[(size_t)eid] == 0.0f) touched_engrams_.push_back(eid);
            exc_buf_[(size_t)eid] += qv * syn.second;
        }
    }

    float best_exc = 0.0f;
    i32 best_eid = -1;
    for (i32 eid : touched_engrams_) {
        const float e = exc_buf_[(size_t)eid];
        exc_buf_[(size_t)eid] = 0.0f;
        if (e > best_exc) {
            best_exc = e;
            best_eid = eid;
        }
    }
    if (out_score) *out_score = best_exc;
    if (best_eid >= 0 && best_exc >= min_sim) {
        out_trace = engrams_[(size_t)best_eid];
        return true;
    }
    return false;
}

void EpisodicCA3::save(std::ostream& os) const {
    const uint32_t magic = 0x43584831u; // "CXH1"
    write_pod(os, magic);
    write_pod(os, dim_);
    const uint32_t ne = (uint32_t)engrams_.size();
    write_pod(os, ne);
    for (const std::string& s : engrams_) {
        const uint32_t len = (uint32_t)s.size();
        write_pod(os, len);
        if (len > 0) os.write(s.data(), len);
    }
    std::vector<i32> non_empty_cols;
    for (int c = 0; c < dim_; ++c) {
        if (!syn_from_concept_[(size_t)c].empty()) non_empty_cols.push_back(c);
    }
    const uint32_t nc = (uint32_t)non_empty_cols.size();
    write_pod(os, nc);
    for (i32 c : non_empty_cols) {
        write_pod(os, c);
        const auto& vec = syn_from_concept_[(size_t)c];
        const uint32_t ns = (uint32_t)vec.size();
        write_pod(os, ns);
        if (ns > 0) {
            os.write(reinterpret_cast<const char*>(vec.data()),
                     (std::streamsize)(ns * sizeof(std::pair<i32, float>)));
        }
    }
}

void EpisodicCA3::load(std::istream& is) {
    uint32_t magic = 0;
    read_pod(is, magic);
    if (magic != 0x43584831u) throw std::runtime_error("formato hipocampal CA3 invalido");
    read_pod(is, dim_);
    uint32_t ne = 0;
    read_pod(is, ne);
    engrams_.assign(ne, {});
    for (uint32_t i = 0; i < ne; ++i) {
        uint32_t len = 0;
        read_pod(is, len);
        engrams_[i].resize(len);
        if (len > 0) is.read(&engrams_[i][0], len);
    }
    syn_from_concept_.assign((size_t)std::max(0, dim_), {});
    uint32_t nc = 0;
    read_pod(is, nc);
    for (uint32_t k = 0; k < nc; ++k) {
        i32 c = 0;
        uint32_t ns = 0;
        read_pod(is, c);
        read_pod(is, ns);
        if (c >= 0 && c < dim_ && ns > 0) {
            auto& vec = syn_from_concept_[(size_t)c];
            vec.resize(ns);
            is.read(reinterpret_cast<char*>(vec.data()),
                    (std::streamsize)(ns * sizeof(std::pair<i32, float>)));
        }
    }
    exc_buf_.assign(engrams_.size(), 0.0f);
    touched_engrams_.clear();
}

// ===========================================================================
// PROYECCION MOTORA COMPACTA BAJO DEMANDA EN FP16 (CompactCorticalReadout, CXK1)
// ===========================================================================
void CompactCorticalReadout::init(int n_features, float lr, int init_slots) {
    F_ = std::max(1, n_features);
    cap_slots_ = (init_slots <= 128) ? 128 : 256;
    n_active_ = 0;
    n_rows_ = 0;
    lr_ = lr;
    consol_pull_ = 0.0f;
    cal_steps_ = 0;
    calibrated_ = false;

    byte_to_slot_.assign(kMaxVocab, -1);
    slot_to_byte_.clear();
    row_id_.assign((size_t)F_, -1);
    row_to_feat_.clear();
    W_fp16_.clear();
    W_anc_fp16_.clear();
    imp_.clear();
    scale_.assign((size_t)F_, 1.0f);
    sq_sum_.assign((size_t)F_, 0.0f);
    b_.assign(kMaxVocab, 0.0f);
    logits_.assign(kMaxVocab, 0.0f);
    probs_.assign(kMaxVocab, 0.0);
    err_.assign(kMaxVocab, 0.0f);
}

size_t CompactCorticalReadout::memory_bytes() const {
    return row_id_.capacity() * sizeof(i32) +
           row_to_feat_.capacity() * sizeof(i32) +
           W_fp16_.capacity() * sizeof(uint16_t) +
           W_anc_fp16_.capacity() * sizeof(uint16_t) +
           imp_.capacity() * sizeof(float) +
           scale_.capacity() * sizeof(float);
}

void CompactCorticalReadout::expand_slots_to_256() {
    if (cap_slots_ >= 256) return;
    const int old_cap = cap_slots_;
    cap_slots_ = 256;
    if (n_rows_ > 0) {
        std::vector<uint16_t> nw(n_rows_ * 256, 0);
        for (size_t r = 0; r < n_rows_; ++r) {
            std::memcpy(&nw[r * 256], &W_fp16_[r * (size_t)old_cap], (size_t)old_cap * sizeof(uint16_t));
        }
        W_fp16_.swap(nw);
        if (!W_anc_fp16_.empty()) {
            std::vector<uint16_t> na(n_rows_ * 256, 0);
            for (size_t r = 0; r < n_rows_; ++r) {
                std::memcpy(&na[r * 256], &W_anc_fp16_[r * (size_t)old_cap], (size_t)old_cap * sizeof(uint16_t));
            }
            W_anc_fp16_.swap(na);
        }
    }
}

int CompactCorticalReadout::ensure_byte_slot(int byte_val) {
    const int b = byte_val & 0xFF;
    if (byte_to_slot_[(size_t)b] >= 0) return byte_to_slot_[(size_t)b];
    if (n_active_ >= cap_slots_) expand_slots_to_256();
    const int sl = n_active_++;
    byte_to_slot_[(size_t)b] = sl;
    slot_to_byte_.push_back(b);
    return sl;
}

i32 CompactCorticalReadout::ensure_row(i32 feat) {
    i32 r = row_id_[(size_t)feat];
    if (r >= 0) return r;
    r = (i32)n_rows_++;
    row_id_[(size_t)feat] = r;
    row_to_feat_.push_back(feat);
    W_fp16_.resize(n_rows_ * (size_t)cap_slots_, 0);
    if (!W_anc_fp16_.empty()) W_anc_fp16_.resize(n_rows_ * (size_t)cap_slots_, 0);
    imp_.push_back(0.0f);
    return r;
}

void CompactCorticalReadout::calibrate_step(const std::vector<std::pair<i32, float>>& feats) {
    if (calibrated_) return;
    cal_steps_++;
    for (const auto& kv : feats) {
        if (kv.first >= 0 && kv.first < F_)
            sq_sum_[(size_t)kv.first] += kv.second * kv.second;
    }
}

void CompactCorticalReadout::freeze_calibration() {
    if (calibrated_) return;
    const float denom = (float)std::max(1L, cal_steps_);
    for (int i = 0; i < F_; ++i) {
        const float rms = std::sqrt(sq_sum_[(size_t)i] / denom);
        scale_[(size_t)i] = (rms > 0.05f) ? std::min(6.0f, 1.0f / rms) : 1.0f;
    }
    sq_sum_.clear();
    sq_sum_.shrink_to_fit();
    calibrated_ = true;
}

void CompactCorticalReadout::predict(const std::vector<std::pair<i32, float>>& feats,
                                     double* probs,
                                     double temperature) const {
    if (n_active_ <= 0) {
        const double u = 1.0 / (double)kMaxVocab;
        for (int i = 0; i < kMaxVocab; ++i) probs[i] = u;
        return;
    }
    scaled_feats_.clear();
    double norm_sq = 0.0;
    for (const auto& kv : feats) {
        const i32 idx = kv.first;
        if (idx < 0 || idx >= F_) continue;
        const float z = kv.second * (calibrated_ ? scale_[(size_t)idx] : 1.0f);
        if (std::fabs(z) < 1e-7f) continue;
        scaled_feats_.push_back({idx, z});
        norm_sq += (double)z * (double)z;
    }
    const float inv_n = norm_sq > 1e-6 ? (float)(1.0 / std::sqrt(norm_sq)) : 1.0f;
    const int C = n_active_;
    for (int s = 0; s < C; ++s) logits_[(size_t)s] = b_[(size_t)s];

    for (const auto& kv : scaled_feats_) {
        const i32 r = row_id_[(size_t)kv.first];
        if (r < 0) continue;
        const float z = kv.second * inv_n;
        const uint16_t* __restrict__ w16 = &W_fp16_[(size_t)r * (size_t)cap_slots_];
        for (int s = 0; s < C; ++s) {
            logits_[(size_t)s] += z * fp16_to_fp32(w16[s]);
        }
    }

    const double inv_t = 1.0 / std::max(0.05, temperature);
    double mx = -1e30;
    for (int s = 0; s < C; ++s) {
        const double v = (double)logits_[(size_t)s] * inv_t;
        if (v > mx) mx = v;
    }
    std::fill(probs, probs + kMaxVocab, 0.0);
    double sum = 0.0;
    for (int s = 0; s < C; ++s) {
        const double e = std::exp(std::max(-60.0, (double)logits_[(size_t)s] * inv_t - mx));
        probs[slot_to_byte_[(size_t)s]] = e;
        sum += e;
    }
    const double inv_s = 1.0 / std::max(1e-30, sum);
    for (int s = 0; s < C; ++s) probs[slot_to_byte_[(size_t)s]] *= inv_s;
}

double CompactCorticalReadout::step(int target_byte,
                                    const std::vector<std::pair<i32, float>>& feats,
                                    bool learn,
                                    float mod) {
    const int tb = target_byte & 0xFF;
    // En inferencia el target no debe ampliar el vocabulario antes de calcular
    // el softmax: eso filtraría conocimiento del byte que se está puntuando.
    int t_slot = byte_to_slot_[(size_t)tb];
    if (t_slot < 0 && learn) t_slot = ensure_byte_slot(tb);

    scaled_feats_.clear();
    double norm_sq = 0.0;
    for (const auto& kv : feats) {
        const i32 idx = kv.first;
        if (idx < 0 || idx >= F_) continue;
        const float z = kv.second * (calibrated_ ? scale_[(size_t)idx] : 1.0f);
        if (std::fabs(z) < 1e-7f) continue;
        scaled_feats_.push_back({idx, z});
        norm_sq += (double)z * (double)z;
    }
    const float inv_n = norm_sq > 1e-6 ? (float)(1.0 / std::sqrt(norm_sq)) : 1.0f;
    const int C = n_active_;
    for (int s = 0; s < C; ++s) logits_[(size_t)s] = b_[(size_t)s];

    for (const auto& kv : scaled_feats_) {
        const i32 r = row_id_[(size_t)kv.first];
        if (r < 0) continue;
        const float z = kv.second * inv_n;
        const uint16_t* __restrict__ w16 = &W_fp16_[(size_t)r * (size_t)cap_slots_];
        for (int s = 0; s < C; ++s) {
            logits_[(size_t)s] += z * fp16_to_fp32(w16[s]);
        }
    }

    float mx = -1e30f;
    for (int s = 0; s < C; ++s) if (logits_[(size_t)s] > mx) mx = logits_[(size_t)s];
    double sum = 0.0;
    for (int s = 0; s < C; ++s) {
        const double e = std::exp((double)(logits_[(size_t)s] - mx));
        probs_[(size_t)s] = e;
        sum += e;
    }
    const double inv_s = 1.0 / std::max(1e-30, sum);
    for (int s = 0; s < C; ++s) probs_[(size_t)s] *= inv_s;

    // Un byte ausente en el vocabulario aprendido recibe una probabilidad piso,
    // sin añadir una clase durante evaluación. En entrenamiento ya se añadió arriba.
    const double p_tgt = (t_slot >= 0) ? std::max(1e-12, probs_[(size_t)t_slot]) : 1e-6;
    const double loss_bits = -std::log2(p_tgt);

    if (learn && lr_ > 0.0f && mod > 0.0f) {
        const float eff_lr = lr_ * mod;
        for (int s = 0; s < C; ++s) {
            const float target = (s == t_slot) ? 1.0f : 0.0f;
            err_[(size_t)s] = ((float)probs_[(size_t)s] - target) * eff_lr;
            b_[(size_t)s] = std::max(-6.0f, std::min(6.0f, b_[(size_t)s] - 0.12f * err_[(size_t)s]));
        }
        const bool has_anc = (!W_anc_fp16_.empty() && consol_pull_ > 0.0f);
        for (const auto& kv : scaled_feats_) {
            const i32 r = ensure_row(kv.first);
            const float z = kv.second * inv_n;
            imp_[(size_t)r] = std::min(4.0f, imp_[(size_t)r] + 0.0005f * std::fabs(z));
            const float pull = has_anc ? (consol_pull_ * imp_[(size_t)r] * eff_lr) : 0.0f;
            uint16_t* __restrict__ w16 = &W_fp16_[(size_t)r * (size_t)cap_slots_];
            const uint16_t* __restrict__ a16 = has_anc ? &W_anc_fp16_[(size_t)r * (size_t)cap_slots_] : nullptr;
            for (int s = 0; s < C; ++s) {
                float w = fp16_to_fp32(w16[s]) - z * err_[(size_t)s];
                if (has_anc) w += pull * (fp16_to_fp32(a16[s]) - w);
                w = std::max(-12.0f, std::min(12.0f, w));
                w16[s] = fp32_to_fp16_sr(w, sr_rng_);
            }
        }
    }
    return loss_bits;
}

void CompactCorticalReadout::consolidate(float strength) {
    consol_pull_ = std::max(0.0f, strength);
    if (consol_pull_ <= 0.0f || W_fp16_.empty()) return;
    if (W_anc_fp16_.size() != W_fp16_.size()) {
        W_anc_fp16_ = W_fp16_;
    } else {
        for (size_t i = 0; i < W_fp16_.size(); ++i) {
            const float wn = fp16_to_fp32(W_fp16_[i]);
            const float wa = fp16_to_fp32(W_anc_fp16_[i]);
            W_anc_fp16_[i] = fp32_to_fp16(0.65f * wn + 0.35f * wa);
        }
    }
}

void CompactCorticalReadout::scale_weights(float factor) {
    for (uint16_t& h : W_fp16_) h = fp32_to_fp16(fp16_to_fp32(h) * factor);
    for (float& x : b_) x *= factor;
}

void CompactCorticalReadout::save(std::ostream& os) const {
    const uint32_t magic = 0x43584B31u; // "CXK1"
    write_pod(os, magic);
    write_pod(os, F_);
    write_pod(os, cap_slots_);
    write_pod(os, n_active_);
    const uint64_t nr = (uint64_t)n_rows_;
    write_pod(os, nr);
    write_pod(os, lr_);
    write_pod(os, consol_pull_);
    write_pod(os, calibrated_);
    write_vec(os, byte_to_slot_);
    write_vec(os, slot_to_byte_);
    write_vec(os, row_to_feat_);
    write_vec(os, W_fp16_);
    write_vec(os, W_anc_fp16_);
    write_vec(os, imp_);
    write_vec(os, scale_);
    write_vec(os, b_);
}

void CompactCorticalReadout::load(std::istream& is) {
    uint32_t magic = 0;
    read_pod(is, magic);
    if (magic != 0x43584B31u) throw std::runtime_error("formato CompactCorticalReadout invalido");
    read_pod(is, F_);
    read_pod(is, cap_slots_);
    read_pod(is, n_active_);
    uint64_t nr = 0;
    read_pod(is, nr);
    n_rows_ = (size_t)nr;
    read_pod(is, lr_);
    read_pod(is, consol_pull_);
    read_pod(is, calibrated_);
    read_vec(is, byte_to_slot_);
    read_vec(is, slot_to_byte_);
    read_vec(is, row_to_feat_);
    read_vec(is, W_fp16_);
    read_vec(is, W_anc_fp16_);
    read_vec(is, imp_);
    read_vec(is, scale_);
    read_vec(is, b_);
    row_id_.assign((size_t)std::max(1, F_), -1);
    for (size_t r = 0; r < row_to_feat_.size(); ++r) {
        const i32 f = row_to_feat_[r];
        if (f >= 0 && f < F_) row_id_[(size_t)f] = (i32)r;
    }
    logits_.assign(kMaxVocab, 0.0f);
    probs_.assign(kMaxVocab, 0.0);
    err_.assign(kMaxVocab, 0.0f);
}

}  // namespace cortex
