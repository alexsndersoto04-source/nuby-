// cortex_lm.cpp — implementacion del motor
//
// Orden de un tick:
//   1. entregar llegadas (con retardo) y actualizar elegibilidad en el evento
//   2. inyectar corriente sensorial si toca
//   3. integrar y decidir disparo (umbral + presupuesto)
//   4. competicion k-WTA: solo sobreviven los k mayores
//   5. propagar con retardo, actualizar trazas y umbral adaptativo
//   6. decaer

#include "cortex_lm.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <numeric>
#include <cstdlib>

namespace cortex {

namespace {
constexpr uint32_t MAGIC = 0x43585431;  // "CXT1"
constexpr uint32_t VERSION = 1;

}  // namespace

Engine::Engine(const Params& p) : p_(p) {
    if (p_.n_layers < 1) throw std::invalid_argument("n_layers >= 1");
    if (p_.layer_size < 8) throw std::invalid_argument("layer_size >= 8");
    if (p_.k_wta < 1 || p_.k_wta > p_.layer_size)
        throw std::invalid_argument("k_wta fuera de rango");
    if (p_.d_max < 1 || p_.d_max > 200)
        throw std::invalid_argument("d_max fuera de rango");
}

size_t Engine::n_synapses() const {
    size_t s = 0;
    for (const auto& L : layers_) s += L.g.n_syn;
    return s;
}

// ---------------------------------------------------------------------------
// Construccion
// ---------------------------------------------------------------------------

void Engine::build_random(unsigned seed) {
    rng_.seed(seed);
    const int N = p_.layer_size;
    const int L = p_.n_layers;

    layers_.clear();
    layers_.resize(L);

    const int total = N * L;
    sensory_.assign(p_.n_chars * BITS_PER_CHAR, 0.0f);
    feedback_.assign(total, 0.0f);

    // B: retroalimentacion fija. -+1 con densidad 10 %, NO se entrena.
    const int per_neur = std::max(4, p_.n_chars / 10);
    B_.assign((size_t)total * p_.n_chars, 0.0f);
    std::uniform_int_distribution<int> chdist(0, p_.n_chars - 1);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    for (int j = 0; j < total; ++j) {
        for (int k = 0; k < per_neur; ++k) {
            int c = chdist(rng_);
            B_[(size_t)j * p_.n_chars + c] = gauss(rng_);
        }
    }

    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    std::lognormal_distribution<float> ldn(0.0f, 0.6f);

    for (int li = 0; li < L; ++li) {
        Layer& Lr = layers_[li];
        Lr.n = N;
        Lr.offset = li * N;
        Lr.v.assign(N, p_.v_reset);
        Lr.theta.assign(N, 0.0f);
        Lr.vtrace.assign(N, 0.0f);
        Lr.rate.assign(N, 0.0f);
        Lr.ctx.assign(N, 0.0f);
        Lr.last_spike.assign(N, -1000000);
        Lr.refr.assign(N, 0);
        Lr.spent.assign(N, 0);
        Lr.win_start.assign(N, 0);
        Lr.spike_count.assign(N, 0);

        // CSR: out-degree = density * N, con retardos log-normales acotados
        const int out_deg = std::max(1, (int)(p_.density * N));
        Lr.g.offsets.assign(N + 1, 0);
        Lr.g.post.clear();
        Lr.g.weight.clear();
        Lr.g.delay.clear();
        Lr.g.sign.clear();
        Lr.is_inh.assign(N, 0);
        Lr.g.post.reserve((size_t)N * out_deg);
        Lr.g.weight.reserve((size_t)N * out_deg);
        Lr.g.delay.reserve((size_t)N * out_deg);

        // marcar interneuronas: las ultimas `inh_frac` de la capa
        const int n_inh = std::max(2, (int)(p_.inh_frac * N));
        for (int j = N - n_inh; j < N; ++j) Lr.is_inh[j] = 1;

        std::uniform_int_distribution<int> nd(0, N - 1);
        for (int pre = 0; pre < N; ++pre) {
            Lr.g.offsets[pre] = (i32)Lr.g.post.size();
            if (Lr.is_inh[pre]) {
                // interneurona: proyeccion divergente a toda la capa, retardo
                // mayor que la excitacion (llega despues, y por eso puede frenar)
                const int fan = out_deg * p_.inh_fanout;
                for (int k = 0; k < fan; ++k) {
                    int post = nd(rng_);
                    Lr.g.post.push_back(post);
                    Lr.g.weight.push_back(p_.inh_weight * (0.6f + 0.8f * uni(rng_)));
                    Lr.g.delay.push_back((uint8_t)p_.inh_delay);
                    Lr.g.sign.push_back(1);
                }
            } else {
                for (int k = 0; k < out_deg; ++k) {
                    int post = nd(rng_);
                    if (post == pre) continue;
                    Lr.g.post.push_back(post);
                    Lr.g.weight.push_back(p_.w_init_scale * (0.5f + 0.5f * uni(rng_)));
                    int d = (int)(ldn(rng_) * p_.d_max * 0.35f);
                    if (d < 1) d = 1;
                    if (d > p_.d_max) d = p_.d_max;
                    Lr.g.delay.push_back((uint8_t)d);
                    Lr.g.sign.push_back(0);
                }
            }
        }
        Lr.g.offsets[N] = (i32)Lr.g.post.size();
        Lr.g.n_syn = Lr.g.post.size();

        Lr.elig.assign(Lr.g.n_syn, 0.0f);
        Lr.fast.assign(Lr.g.n_syn, 0.0f);
        Lr.active.clear();
        Lr.active_next.clear();
        Lr.touch.clear();
        Lr.in_active.assign(N, 0);
        Lr.in_touch.assign(Lr.g.n_syn, 0);
    }

    // Readout: n_chars x n_features
    const int F = p_.n_features();
    R_.assign((size_t)p_.n_chars * F, 0.0f);
    R_m_.assign(R_.size(), 0.0f);
    R_v_.assign(R_.size(), 0.0f);
    std::normal_distribution<float> rn(0.0f, 0.02f);
    for (auto& x : R_) x = rn(rng_);

    logits_.assign(p_.n_chars, 0.0f);
    act_now_.assign(total, 0.0f);
    reset_state();
}

void Engine::reset_state() {
    for (auto& L : layers_) {
        std::fill(L.v.begin(), L.v.end(), p_.v_reset);
        std::fill(L.theta.begin(), L.theta.end(), 0.0f);
        std::fill(L.vtrace.begin(), L.vtrace.end(), 0.0f);
        std::fill(L.rate.begin(), L.rate.end(), 0.0f);
        std::fill(L.ctx.begin(), L.ctx.end(), 0.0f);
        std::fill(L.refr.begin(), L.refr.end(), 0);
        std::fill(L.spent.begin(), L.spent.end(), 0);
        std::fill(L.win_start.begin(), L.win_start.end(), 0);
        std::fill(L.elig.begin(), L.elig.end(), 0.0f);
        L.active.clear();
        L.active_next.clear();
        L.touch.clear();
        std::fill(L.in_active.begin(), L.in_active.end(), 0);
        std::fill(L.in_touch.begin(), L.in_touch.end(), 0);
    }
    std::fill(act_now_.begin(), act_now_.end(), 0.0f);
    for (auto& q : queues_) {
        for (auto& b : q.buckets) b.clear();
        q.head = 0;
    }
    t_ = 0;
    symbols_since_consol_ = 0;
}

// ---------------------------------------------------------------------------
// Simulacion
// ---------------------------------------------------------------------------

void Engine::begin_symbol() {
    for (auto& L : layers_) std::fill(L.rate.begin(), L.rate.end(), 0.0f);
}

void Engine::feed(int ch) {
    if (ch < 0 || ch >= p_.n_chars) throw std::out_of_range("char fuera de vocabulario");
    begin_symbol();

    // corriente sensorial: los bits a 1 del codigo del caracter empujan un
    // conjunto fijo de neuronas de la capa 0.
    Layer& L0 = layers_[0];
    const int total = n_neurons();
    std::uniform_int_distribution<int> nd(0, L0.n - 1);

    // proyeccion fija y determinista: misma semilla -> mismo mapa
    std::mt19937 enc((uint32_t)(p_.seed * 7919u + (uint32_t)ch * 104729u));
    for (int bit = 0; bit < BITS_PER_CHAR; ++bit) {
        if (!((ch >> bit) & 1)) continue;
        for (int k = 0; k < 24; ++k) {
            int j = (int)(enc() % (uint32_t)L0.n);
            L0.v[j] += p_.input_gain;
            if (!L0.in_active[j]) { L0.in_active[j] = 1; L0.active.push_back(j); }
        }
    }

    for (int s = 0; s < p_.ticks_per_char; ++s) {
        step();
    }

    // memoria de contexto: promedio decaido de la tasa. Es lo que permite al
    // readout ver mas alla del simbolo actual (si no, solo aprende bigramas).
    const float k = 0.25f;
    for (auto& L : layers_) {
        for (int j = 0; j < L.n; ++j) {
            L.ctx[j] = (1.0f - k) * L.ctx[j] + k * L.rate[j];
        }
    }

    // activacion media del simbolo -> features del readout
    symbols_++;
    symbols_since_consol_++;
    (void)total;
}

int Engine::step() {
    if ((int)queues_.size() != p_.n_layers) {
        queues_.resize(p_.n_layers);
        for (auto& q : queues_) q.init(p_.d_max);
    }

    const float alpha_m = p_.alpha_m();
    const float alpha_e = p_.alpha_e();
    int fired_total = 0;

    for (int li = 0; li < p_.n_layers; ++li) {
        Layer& L = layers_[li];
        auto& q = queues_[li];

        // --- 1. llegadas (solo las de este tick) ---
        for (const Ev& e : q.drain()) {
            const float cur = L.g.weight[e.syn] + L.fast[e.syn];
            if (L.g.sign[e.syn]) {
                L.v[e.post] -= cur;
            } else {
                L.v[e.post] += cur;
            }
            if (!L.in_active[e.post]) {
                L.in_active[e.post] = 1;
                L.active.push_back(e.post);
            }
            // elegibilidad local, actualizada EN EL EVENTO
            float vp = L.v[e.post];
            float psi = (vp > p_.v_th) ? 1.0f
                        : std::max(0.0f, 1.0f - (p_.v_th - vp) * 4.0f);
            float e_new = L.elig[e.syn] + psi;
            if (e_new > p_.elig_clip) e_new = p_.elig_clip;
            else if (e_new < -p_.elig_clip) e_new = -p_.elig_clip;
            L.elig[e.syn] = e_new;
            if (!L.in_touch[e.syn]) {
                L.in_touch[e.syn] = 1;
                L.touch.push_back(e.syn);
            }
        }

        // --- 2/3. integracion: SOLO las neuronas activas ---
        auto& cand = cand_;
        cand.clear();
        L.active_next.clear();

        for (i32 j : L.active) {
            if (L.refr[j] > 0) {
                L.refr[j]--;
                L.v[j] = p_.v_reset;
                L.in_active[j] = 0;
                continue;
            }
            if (t_ - L.win_start[j] >= p_.budget_win) {
                L.win_start[j] = t_;
                L.spent[j] = 0;
            }
            const float thr_j = p_.v_th + L.theta[j]
                                - (L.is_inh[j] ? p_.inh_bias : 0.0f);
            if (L.v[j] >= thr_j && L.spent[j] < p_.budget) {
                cand.emplace_back(L.v[j], j);
                continue;   // no decae: va a disparar
            }
            L.v[j] = p_.v_reset + (L.v[j] - p_.v_reset) * alpha_m;
            if (L.v[j] < 1e-4f) {
                L.v[j] = p_.v_reset;
                L.in_active[j] = 0;
            } else {
                L.active_next.push_back(j);
            }
        }

        // --- 4. competicion k-WTA ---
        if ((int)cand.size() > p_.k_wta) {
            std::nth_element(cand.begin(), cand.begin() + p_.k_wta, cand.end(),
                             [](const auto& a, const auto& b) { return a.first > b.first; });
            cand.resize(p_.k_wta);
        }

        // --- 5. disparo y propagacion ---
        for (const auto& pr : cand) {
            int j = pr.second;
            L.v[j] = p_.v_reset;
            L.refr[j] = p_.refractory;
            L.last_spike[j] = t_;
            L.spent[j]++;
            L.spike_count[j]++;
            L.rate[j] += 1.0f;
            L.vtrace[j] = 1.0f;
            L.theta[j] += p_.theta_step;
            act_now_[L.offset + j] = 1.0f;
            L.active_next.push_back(j);
            L.in_active[j] = 1;

            const i32 lo = L.g.offsets[j];
            const i32 hi = L.g.offsets[j + 1];
            for (i32 s = lo; s < hi; ++s) {
                q.push(t_ + L.g.delay[s], Ev{L.g.post[s], s});
            }
        }
        fired_total += (int)cand.size();

        // --- 6. decaimiento de trazas: SOLO las neuronas activas ---
        for (i32 j : L.active_next) {
            L.vtrace[j] *= 0.90f;
            L.theta[j] *= 0.9995f;
            if (L.vtrace[j] < 1e-4f) L.vtrace[j] = 0.0f;
        }

        L.active.swap(L.active_next);
    }

    // decaimiento de elegibilidad: SOLO las sinapsis con traza viva
    for (auto& L : layers_) {
        if (L.touch.empty()) continue;
        size_t keep = 0;
        for (size_t k = 0; k < L.touch.size(); ++k) {
            i32 s = L.touch[k];
            float x = L.elig[s] * alpha_e;
            if (std::fabs(x) < 1e-5f) {
                L.elig[s] = 0.0f;
                L.in_touch[s] = 0;
            } else {
                L.elig[s] = x;
                L.touch[keep++] = s;
            }
        }
        L.touch.resize(keep);
    }

    t_++;
    spikes_ += fired_total;
    return fired_total;
}

// ---------------------------------------------------------------------------
// Lectura
// ---------------------------------------------------------------------------

void Engine::feature_vector(std::vector<float>& out) const {
    // Por capa: tasa del simbolo actual (que dijo) y contexto decaido
    // (que venia diciendo). El sesgo cierra el vector.
    const int N = p_.layer_size;
    const int F = p_.n_features();
    out.assign(F, 0.0f);
    const float inv = 1.0f / std::max(1, p_.ticks_per_char);
    int idx = 0;
    for (int li = 0; li < p_.n_layers; ++li) {
        const Layer& L = layers_[li];
        for (int j = 0; j < N && idx < F - 1; ++j) out[idx++] = L.rate[j] * inv;
        for (int j = 0; j < N && idx < F - 1; ++j) out[idx++] = L.ctx[j] * inv;
    }
    out[F - 1] = 1.0f;
    // normalizacion L2: el readout no debe depender de la escala de actividad
    float nrm = 0.0f;
    for (int i = 0; i < F - 1; ++i) nrm += out[i] * out[i];
    nrm = std::sqrt(nrm);
    if (nrm > 1e-6f) {
        const float inv2 = 1.0f / nrm;
        for (int i = 0; i < F - 1; ++i) out[i] *= inv2;
    }
}

void Engine::features(std::vector<float>& out) const { feature_vector(out); }

void Engine::readout(float* logits) const {
    std::vector<float> f;
    feature_vector(f);
    const int F = p_.n_features();
    for (int k = 0; k < p_.n_chars; ++k) {
        float acc = 0.0f;
        const float* row = &R_[(size_t)k * F];
        for (int i = 0; i < F; ++i) acc += row[i] * f[i];
        logits[k] = acc;
    }
}

// ---------------------------------------------------------------------------
// Aprendizaje
// ---------------------------------------------------------------------------

void Engine::learn(const float* error) {
    const int total = n_neurons();

    // 1. difundir el error a cada neurona por la matriz fija B
    for (int j = 0; j < total; ++j) {
        float acc = 0.0f;
        const float* row = &B_[(size_t)j * p_.n_chars];
        for (int k = 0; k < p_.n_chars; ++k) acc += row[k] * error[k];
        feedback_[j] = acc;
    }

    // 2. regla de tres factores: dw = -eta * señal(post) * elegibilidad
    //    CORTEX_READOUT_ONLY=1 congela la recurrencia: sirve para separar el
    //    error del readout del error de la regla local. Es un instrumento de
    //    diagnostico, no una opcion de produccion.
    static const bool readout_only = std::getenv("CORTEX_READOUT_ONLY") != nullptr;
    for (auto& L : layers_) {
        if (readout_only) break;
        for (size_t s = 0; s < L.g.n_syn; ++s) {
            float e = L.elig[s];
            if (e == 0.0f) continue;
            int post = L.g.post[s];
            float sig = feedback_[L.offset + post];
            if (L.g.sign[s]) continue;   // el freno no se aprende: sostiene la red
            float dw = -p_.eta * sig * e;
            float w = L.g.weight[s] + dw;
            if (w < 0.0f) w = 0.0f;
            else if (w > p_.w_max) w = p_.w_max;
            L.g.weight[s] = w;
        }
    }

    // 3. readout con Adam (es un clasificador lineal, aqui si vale)
    std::vector<float> f;
    feature_vector(f);
    const int F = p_.n_features();
    const float lr = 8e-3f, b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    const float gclip = 1.0f;   // recorte del gradiente: sin esto diverge
    static thread_local long step = 0;
    step++;
    for (int k = 0; k < p_.n_chars; ++k) {
        float g = error[k];
        if (g == 0.0f) continue;
        for (int i = 0; i < F; ++i) {
            size_t idx = (size_t)k * F + i;
            float grad = g * f[i];
            if (grad > gclip) grad = gclip;
            else if (grad < -gclip) grad = -gclip;
            R_m_[idx] = b1 * R_m_[idx] + (1 - b1) * grad;
            R_v_[idx] = b2 * R_v_[idx] + (1 - b2) * grad * grad;
            float mh = R_m_[idx] / (1 - std::pow(b1, (float)step));
            float vh = R_v_[idx] / (1 - std::pow(b2, (float)step));
            float nw = R_[idx] - lr * mh / (std::sqrt(vh) + eps);
            if (nw > 8.0f) nw = 8.0f;          // pesos acotados: robustez
            else if (nw < -8.0f) nw = -8.0f;
            R_[idx] = nw;
        }
    }
}

void Engine::consolidate() {
    // Homeostasis con conciencia del grado.
    //
    // El error ingenuo es normalizar la suma de pesos entrantes a una constante
    // global. Una neurona que recibe de 200 sinapsis queda con 5e-3 por sinapsis
    // y deja de disparar; la red se apaga sola. Lo correcto (y lo que hace una
    // neurona real) es defender la corriente que necesita para llegar al umbral,
    // que depende de CUANTAS entradas tiene: objetivo = norm * sqrt(indegree).
    //
    // Se usa una forma cerrada y exacta: cada peso se escala por
    // (objetivo / suma_actual) elevado a 1/indegree, de modo que la suma de los
    // pesos escalados es exactamente el objetivo. Sin bucles ni iteraciones.
    for (auto& L : layers_) {
        std::vector<float> in_sum(L.n, 0.0f);
        std::vector<i32> in_cnt(L.n, 0);
        for (size_t s = 0; s < L.g.n_syn; ++s) {
            if (L.g.sign[s]) continue;
            in_sum[L.g.post[s]] += L.g.weight[s];
            in_cnt[L.g.post[s]]++;
        }
        std::vector<float> factor(L.n, 1.0f);
        for (int j = 0; j < L.n; ++j) {
            if (in_cnt[j] == 0 || in_sum[j] < 1e-8f) continue;
            float target = p_.weight_norm * std::sqrt((float)in_cnt[j]);
            float r = target / in_sum[j];
            // r^(1/cnt): el producto de los cnt pesos escalados = objetivo
            float f = std::exp(std::log(r) / (float)in_cnt[j]);
            if (f < 0.5f) f = 0.5f;
            else if (f > 2.0f) f = 2.0f;
            factor[j] = f;
        }
        for (size_t s = 0; s < L.g.n_syn; ++s) {
            if (L.g.sign[s]) continue;
            float w = L.g.weight[s] * factor[L.g.post[s]];
            if (w > p_.w_max) w = p_.w_max;
            L.g.weight[s] = w;
            L.fast[s] *= (1.0f - p_.consol_gain);
        }
    }
    symbols_since_consol_ = 0;
    reset_state();
}

// ---------------------------------------------------------------------------
// Persistencia (binario con cabecera versionada)
// ---------------------------------------------------------------------------

void Engine::save(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("no se pudo abrir para escribir: " + path);
    f.write((const char*)&MAGIC, 4);
    f.write((const char*)&VERSION, 4);
    f.write((const char*)&p_, sizeof(Params));
    int nl = p_.n_layers;
    f.write((const char*)&nl, 4);
    for (const auto& L : layers_) {
        size_t ns = L.g.n_syn;
        f.write((const char*)&ns, 8);
        f.write((const char*)L.g.offsets.data(), sizeof(i32) * L.g.offsets.size());
        f.write((const char*)L.g.post.data(), sizeof(i32) * ns);
        f.write((const char*)L.g.weight.data(), sizeof(float) * ns);
        f.write((const char*)L.g.delay.data(), ns);
        f.write((const char*)L.g.sign.data(), ns);
        f.write((const char*)L.is_inh.data(), L.n);
        f.write((const char*)L.spike_count.data(), sizeof(i32) * L.n);
    }
    size_t rf = R_.size();
    f.write((const char*)&rf, 8);
    f.write((const char*)R_.data(), sizeof(float) * rf);
    size_t bf = B_.size();
    f.write((const char*)&bf, 8);
    f.write((const char*)B_.data(), sizeof(float) * bf);
}

void Engine::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("no se pudo abrir: " + path);
    uint32_t mg = 0, ver = 0;
    f.read((char*)&mg, 4);
    f.read((char*)&ver, 4);
    if (mg != MAGIC) throw std::runtime_error("no es un archivo CORTEX");
    if (ver != VERSION) throw std::runtime_error("version incompatible");
    Params loaded;
    f.read((char*)&loaded, sizeof(Params));
    p_ = loaded;
    int nl = 0;
    f.read((char*)&nl, 4);
    build_random(p_.seed);
    layers_.resize(nl);
    for (int li = 0; li < nl; ++li) {
        Layer& L = layers_[li];
        L.n = p_.layer_size;
        L.offset = li * p_.layer_size;
        size_t ns = 0;
        f.read((char*)&ns, 8);
        L.g.offsets.resize(L.n + 1);
        f.read((char*)L.g.offsets.data(), sizeof(i32) * L.g.offsets.size());
        L.g.post.resize(ns);
        L.g.weight.resize(ns);
        L.g.delay.resize(ns);
        L.g.sign.resize(ns);
        L.is_inh.resize(L.n);
        L.g.n_syn = ns;
        f.read((char*)L.g.post.data(), sizeof(i32) * ns);
        f.read((char*)L.g.weight.data(), sizeof(float) * ns);
        f.read((char*)L.g.delay.data(), ns);
        f.read((char*)L.g.sign.data(), ns);
        f.read((char*)L.is_inh.data(), L.n);
        L.spike_count.resize(L.n);
        f.read((char*)L.spike_count.data(), sizeof(i32) * L.n);
        L.elig.assign(ns, 0.0f);
        L.fast.assign(ns, 0.0f);
    }
    size_t rf = 0;
    f.read((char*)&rf, 8);
    R_.resize(rf);
    R_m_.assign(rf, 0.0f);
    R_v_.assign(rf, 0.0f);
    f.read((char*)R_.data(), sizeof(float) * rf);
    size_t bf = 0;
    f.read((char*)&bf, 8);
    B_.resize(bf);
    f.read((char*)B_.data(), sizeof(float) * bf);
    feedback_.assign(n_neurons(), 0.0f);
    logits_.assign(p_.n_chars, 0.0f);
    act_now_.assign(n_neurons(), 0.0f);
    sensory_.assign(p_.n_chars * BITS_PER_CHAR, 0.0f);
    reset_state();
}

// ---------------------------------------------------------------------------

Engine::Stats Engine::stats() const {
    Stats s;
    s.ticks = t_;
    s.spikes = spikes_;
    s.symbols = symbols_;
    double wsum = 0;
    size_t wcount = 0;
    int used = 0;
    for (const auto& L : layers_) {
        for (float w : L.g.weight) { wsum += w; wcount++; }
        for (int c : L.spike_count) if (c > 0) used++;
    }
    s.mean_w = wcount ? (float)(wsum / wcount) : 0.0f;
    s.used_neurons = used;
    return s;
}

}  // namespace cortex
