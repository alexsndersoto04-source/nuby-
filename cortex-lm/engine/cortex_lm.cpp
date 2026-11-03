// cortex_lm.cpp — implementacion del nucleo CORTEX-LM.
//
// Dinamica (una neurona LIF con fatiga, sesgo y freno proporcional):
//
//   v(t) = v_reset + (v(t-1) - v_reset) * alpha_m + I_in(t) + I_rec(t) - I_inh(t) + bias
//   si v >= v_th + theta:  dispara, v = v_reset, theta += theta_step
//   theta *= theta_decay  en cada tick
//
// Lo que cambia respecto a la version auditada, y por que:
//
//  1. LOS PESOS SE DERIVAN, NO SE ELIGEN. Se fijan para que la corriente
//     recurrente total, cuando esta activa la fraccion `active_frac` de la capa,
//     valga `w_scale * v_th`. Antes sumaban 0.80 por neurona con un umbral de
//     0.60, o sea que la red se autoexcitaba sin ayuda.
//
//  2. LA ENTRADA MANDA. `input_gain > v_th`: un simbolo solo, sin recurrencia,
//     hace disparar a su poblacion. Se verifica en el arranque.
//
//  3. EL FRENO ES PROPORCIONAL A LA ACTIVIDAD, no un numero fijo de ganadores.
//     Cada disparo resta `inh_gain` a `inh_fanout` neuronas al azar, con
//     retardo. Cuanto mas activa la capa, mas freno: eso da un punto fijo.
//
//  4. LA MEMORIA ES LA FATIGA. La neurona que dispara tarda en recuperarse, y
//     el patron de fatiga codifica que acaba de ocurrir. Es el mecanismo de
//     memoria de trabajo de la corteza, y es local: no necesita gradientes.

#include "cortex_lm.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <cstdlib>

namespace cortex {

namespace {
constexpr i32 kInhSyn = -1;   // evento de freno, no corresponde a una sinapsis
}

Engine::Engine(const Params& p) : p_(p) {
    if (p_.n_layers < 1) throw std::invalid_argument("n_layers >= 1");
    if (p_.layer_size < 8) throw std::invalid_argument("layer_size >= 8");
    if (p_.d_max < 1) throw std::invalid_argument("d_max >= 1");
    if (p_.ticks_per_char < 1) throw std::invalid_argument("ticks_per_char >= 1");
    ctx_hist_.assign((size_t)p_.n_ctx * p_.n_chars, 0.0f);
}

void Engine::build_sensory_map() {
    // Mapa fijo: cada simbolo enciende un parche contiguo desplazado mas un
    // conjunto pseudoaleatorio propio. Determinista por semilla.
    const int N = p_.layer_size;
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

        // Corriente GRADUADA por conexion. Sin esto la poblacion entera recibe
        // exactamente lo mismo, responde en bloque (todo o nada) y la fatiga la
        // apaga completa. Con gradiente, la fraccion que dispara codifica la
        // fuerza del estimulo y, con la fatiga, su antiguedad.
        std::uniform_real_distribution<float> uu(1.0f - p_.input_spread * 0.5f,
                                                 1.0f + p_.input_spread * 0.5f);
        for (int i = 0; i < per; ++i) dw[i] = p_.input_gain * uu(enc);
    }

    // Canal de memoria: proyeccion independiente, mas ancha y mas fuerte.
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

    const int N = p_.layer_size;
    const int L = p_.n_layers;
    layers_.clear();
    layers_.resize(L);
    queues_.clear();
    queues_.resize(L);

    n_neurons_ = N * L;
    feedback_.assign(n_neurons_, 0.0f);
    fb_prev_.assign(n_neurons_, 0.0f);

    // B: retroalimentacion fija, no se entrena. Conecta cada neurona con una
    // muestra de la salida y permite difundir la senal de error sin gradientes.
    const int per = std::max(2, p_.n_chars / 16);
    std::uniform_int_distribution<int> chd(0, p_.n_chars - 1);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    B_.assign((size_t)n_neurons_ * p_.n_chars, 0.0f);
    for (int j = 0; j < n_neurons_; ++j)
        for (int k = 0; k < per; ++k)
            B_[(size_t)j * p_.n_chars + chd(rng_)] = gauss(rng_);

    std::uniform_int_distribution<int> nd(0, N - 1);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);

    for (int li = 0; li < L; ++li) {
        Layer& Lr = layers_[li];
        Lr.n = N;
        Lr.offset = li * N;
        Lr.v.assign(N, p_.v_reset);
        Lr.theta.assign(N, 0.0f);
        Lr.bias.assign(N, 0.0f);
        Lr.rate.assign(N, 0.0f);
        Lr.rate_last.assign(N, 0.0f);
        Lr.refr.assign(N, 0);
        Lr.spike_count.assign(N, 0);
        Lr.active.assign(N, 0);
        Lr.active_list.clear();

        // --- pesos derivados del grado ---
        // Corriente que aporta UN disparo a UN destino, si disparan
        // active_frac * N neuronas, debe sumar w_scale * v_th.
        const int out_deg = std::max(1, (int)(p_.density * N));
        const double fan_in_frac = (double)out_deg / N;            // prob. de conexion
        const double active_n = std::max(1.0, (double)p_.active_frac * N);
        const double contrib = fan_in_frac * active_n;             // llegadas esperadas
        const double wnum = (double)p_.w_scale * (double)p_.v_th;
        const float w = (float)(wnum / std::max(1.0, contrib));

        Lr.g.offsets.assign(N + 1, 0);
        Lr.g.post.clear();
        Lr.g.weight.clear();
        Lr.g.delay.clear();
        Lr.g.post.reserve((size_t)N * out_deg);
        Lr.g.weight.reserve((size_t)N * out_deg);
        Lr.g.w_init.reserve((size_t)N * out_deg);
        Lr.g.delay.reserve((size_t)N * out_deg);

        std::uniform_int_distribution<int> dd(1, std::max(1, p_.d_max));
        for (int pre = 0; pre < N; ++pre) {
            Lr.g.offsets[pre] = (i32)Lr.g.post.size();
            for (int k = 0; k < out_deg; ++k) {
                int post = nd(rng_);
                if (post == pre) continue;
                Lr.g.post.push_back(post);
                Lr.g.weight.push_back(w * (0.6f + 0.8f * uni(rng_)));
                Lr.g.w_init.push_back(Lr.g.weight.back());
                Lr.g.delay.push_back((uint8_t)dd(rng_));
            }
        }
        Lr.g.offsets[N] = (i32)Lr.g.post.size();
        Lr.g.n_syn = Lr.g.post.size();
        Lr.elig.assign(Lr.g.n_syn, 0.0f);
        Lr.pre_trace.assign(N, 0.0f);
        Lr.alpha_elig = std::exp(-1.0f / std::max(1.0f, p_.tau_elig));

        queues_[li].init(p_.d_max);
    }

    const int F = p_.n_features_max();
    R_.assign((size_t)p_.n_chars * F, 0.0f);
    R_m_.assign(R_.size(), 0.0f);
    R_v_.assign(R_.size(), 0.0f);
    std::normal_distribution<float> rn(0.0f, 0.01f);
    for (auto& x : R_) x = rn(rng_);

    logits_.assign(p_.n_chars, 0.0f);
    mu_.assign(F, 0.0f);
    sd_.assign(F, 1.0f);
    logits_b_.assign(p_.n_chars, 0.0f);
    reset_state();
}

size_t Engine::n_synapses() const {
    size_t s = 0;
    for (const auto& L : layers_) s += L.g.n_syn;
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
        std::fill(L.elig.begin(), L.elig.end(), 0.0f);
        std::fill(L.pre_trace.begin(), L.pre_trace.end(), 0.0f);
        L.active_list.clear();
    }
    for (auto& q : queues_) {
        for (auto& b : q.buckets) b.clear();
        q.head = 0;
    }
    std::fill(ctx_hist_.begin(), ctx_hist_.end(), 0.0f);
    std::fill(feedback_.begin(), feedback_.end(), 0.0f);
    std::fill(fb_prev_.begin(), fb_prev_.end(), 0.0f);
    inh_prev_ = 0.0f;
    t_ = 0;
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
        L0.v[j] += dw[i];
        if (!L0.active[j]) { L0.active[j] = 1; L0.active_list.push_back(j); }
    }
}

void Engine::inject_mem(int ch) {
    if (ch < 0 || ch >= p_.n_chars) throw std::out_of_range("simbolo fuera de rango");
    Layer& L0 = layers_[0];
    const int mp = p_.mem_patch > 0 ? p_.mem_patch : p_.patch_size() * 2;
    const int* dst = &memmap_[(size_t)ch * mp];
    const float* dw = &memmap_w_[(size_t)ch * mp];
    for (int i = 0; i < mp; ++i) L0.v[dst[i]] += dw[i];
}

int Engine::step() {
    const float am = p_.alpha_m();
    int fired = 0;

    // La traza presinaptica decae una vez por tick, antes de la decision.
    for (auto& L : layers_)
        for (auto& tr : L.pre_trace) tr *= L.alpha_elig;

    for (size_t li = 0; li < layers_.size(); ++li) {
        Layer& L = layers_[li];
        DelayQueue& q = queues_[li];

        // 1. llegadas programadas para este tick.
        // El cubo DEBE vaciarse al drenarlo. Antes no se vaciaba: los eventos se
        // reentregaban cada vez que el cabezal volvia a ese cubo (cada `span`
        // ticks), acumulandose sin limite. Con w_scale=0 esos eventos sumaban
        // cero y el fallo era invisible; con recurrencia real saturaban la capa
        // (1024 de 1024 neuronas disparando cada tick) y hacian crecer la cola
        // hasta agotar la memoria.
        std::vector<DelayQueue::Ev>& arrived = q.drain((int)t_);
        for (const auto& ev : arrived) {
            if (ev.syn == kInhSyn) {
                L.v[ev.post] -= p_.inh_gain;
                if (L.v[ev.post] < p_.v_reset) L.v[ev.post] = p_.v_reset;
            } else {
                L.v[ev.post] += L.g.weight[ev.syn];
            }
        }
        arrived.clear();

        // corriente inhibitoria global: proporcional a los disparos del tick
        // anterior y con decaimiento. Es lo que da un punto fijo al regimen.
        const float i_inh = p_.inh_gain * inh_prev_;

        // 2. relajacion de la fatiga y de la membrana
        for (int j = 0; j < L.n; ++j) {
            L.theta[j] *= p_.theta_decay;
            if (L.theta[j] < 1e-4f) L.theta[j] = 0.0f;
            if (L.refr[j] > 0) { L.refr[j]--; continue; }
            L.v[j] = (L.v[j] - p_.v_reset) * am + p_.v_reset + L.bias[j] - i_inh;
            if (L.v[j] < 1e-4f) L.v[j] = p_.v_reset;
        }

        // 3. decision y disparo
        for (int j = 0; j < L.n; ++j) {
            if (L.refr[j] > 0) continue;
            if (L.v[j] < p_.v_th + L.theta[j]) continue;

            L.v[j] = p_.v_reset;
            L.refr[j] = p_.refractory;
            L.rate[j] += 1.0f;
            L.spike_count[j]++;
            L.theta[j] = std::min(p_.theta_max, L.theta[j] + p_.theta_step);
            L.pre_trace[j] = 1.0f;      // marca temporal del disparo
            fired++;

            for (i32 s = L.g.offsets[j]; s < L.g.offsets[j + 1]; ++s)
                q.push((int)t_ + L.g.delay[s], DelayQueue::Ev{L.g.post[s], s});


        }
    }

    // memoria corta de la actividad: el freno del proximo tick
    inh_prev_ = inh_prev_ * std::exp(-1.0f / std::max(1.0f, p_.inh_tau)) + (float)fired;

    t_++;
    spikes_ += fired;
    return fired;
}

void Engine::end_symbol() {
    // homeostasis: la actividad media por neurona debe acercarse a target_rate.
    // Se mide en disparos por simbolo (ticks_per_char ticks).
    const float want = p_.target_rate * p_.ticks_per_char;
    for (auto& L : layers_) {
        L.rate_last = L.rate;                 // perfil del simbolo que se cierra
        for (int j = 0; j < L.n; ++j) {
            const float diff = want - L.rate[j];
            L.bias[j] += p_.homeo_lr * diff;
            L.bias[j] = std::max(-p_.bias_max, std::min(p_.bias_max, L.bias[j]));
            L.rate[j] = 0.0f;
        }
    }
    // desplazar historial sensorial
    if (p_.n_ctx > 1)
        std::memmove(&ctx_hist_[p_.n_chars], &ctx_hist_[0],
                     sizeof(float) * (size_t)p_.n_chars * (p_.n_ctx - 1));
    std::fill(ctx_hist_.begin(), ctx_hist_.begin() + p_.n_chars, 0.0f);
    symbols_++;
}

void Engine::feed(int ch) {
    if (ch < 0 || ch >= p_.n_chars) throw std::out_of_range("simbolo fuera de rango");
    ctx_hist_[ch] = 1.0f;
    inject_only(ch);
    for (int s = 0; s < p_.ticks_per_char; ++s) step();
    end_symbol();
}

void Engine::ctx_features(std::vector<float>& out) const {
    const int F = p_.n_ctx_features();
    out.assign(F, 0.0f);
    for (int i = 0; i < F - 1 && i < (int)ctx_hist_.size(); ++i) out[i] = ctx_hist_[i];
    out[F - 1] = 1.0f;
}

void Engine::rate_features(std::vector<float>& out) const {
    const int F = p_.n_layers * p_.layer_size;
    out.assign(F, 0.0f);
    int idx = 0;
    for (const auto& L : layers_) {
        float nrm = 0.0f;
        for (int j = 0; j < L.n; ++j) nrm += L.rate[j] * L.rate[j];
        nrm = std::sqrt(nrm);
        const float g = nrm > 1e-6f ? 1.0f / nrm : 0.0f;
        for (int j = 0; j < L.n; ++j) out[idx++] = L.rate[j] * g;
    }
}

void Engine::last_features(std::vector<float>& out) const {
    const int F = p_.n_layers * p_.layer_size;
    out.assign(F, 0.0f);
    int idx = 0;
    for (const auto& L : layers_) {
        float nrm = 0.0f;
        for (int j = 0; j < L.n; ++j) nrm += L.rate_last[j] * L.rate_last[j];
        nrm = std::sqrt(nrm);
        const float g = nrm > 1e-6f ? 1.0f / nrm : 0.0f;
        for (int j = 0; j < L.n; ++j) out[idx++] = L.rate_last[j] * g;
    }
}

void Engine::theta_features(std::vector<float>& out) const {
    const int F = p_.n_layers * p_.layer_size;
    out.assign(F, 0.0f);
    int idx = 0;
    for (const auto& L : layers_) {
        float nrm = 0.0f;
        for (int j = 0; j < L.n; ++j) nrm += L.theta[j] * L.theta[j];
        nrm = std::sqrt(nrm);
        const float g = nrm > 1e-6f ? 1.0f / nrm : 0.0f;
        for (int j = 0; j < L.n; ++j) out[idx++] = L.theta[j] * g;
    }
}

// Normalizacion por dimension: quita la media y escala a varianza unitaria.
// Sin esto el readout no puede ver dimensiones pequenas: al normalizar el
// vector completo, 1022 dimensiones a cero y 2 a 0.7 dejan a esas dos con
// practicamente todo el peso, y las muertas igual contribuyen al sesgo.
void Engine::normalize(const std::vector<float>& in, std::vector<float>& out) const {
    const int F = (int)in.size();
    out.assign(F, 0.0f);
    if (mu_.size() != (size_t)F) return;
    for (int i = 0; i < F; ++i) {
        const float sd = sd_[i];
        out[i] = sd > 1e-6f ? (in[i] - mu_[i]) / sd : 0.0f;
    }
}

void Engine::update_norm(const std::vector<float>& f) {
    const int F = (int)f.size();
    if (mu_.size() != (size_t)F) { mu_.assign(F, 0.0f); sd_.assign(F, 1.0f); return; }
    const float a = 0.001f;
    for (int i = 0; i < F; ++i) {
        const float d = f[i] - mu_[i];
        mu_[i] += a * d;
        sd_[i] = std::sqrt((1 - a) * (sd_[i] * sd_[i] + a * d * d));
        // El suelo NO puede ser diminuto: las features one-hot estan casi
        // siempre a cero, su desviacion se hunde y z=(x-mu)/sd se dispara a
        // 1000, con lo que el readout solo ve saturacion. Con suelo 1.0 la
        // dimension activa vale ~1 y la inactiva ~0: escala util.
        if (sd_[i] < 1.0f) sd_[i] = 1.0f;
    }
}

void Engine::readout(const std::vector<float>& f, float* logits) const {
    const int F = (int)f.size();
    const int stride = p_.n_features_max();   // las filas se guardan con ancho fijo
    for (int k = 0; k < p_.n_chars; ++k) {
        float acc = 0.0f;
        const float* row = &R_[(size_t)k * stride];
        for (int i = 0; i < F; ++i) acc += row[i] * f[i];
        logits[k] = acc + (logits_b_.empty() ? 0.0f : logits_b_[k]);
    }
}

void Engine::learn(const float* error, const std::vector<float>& f) {
    // 1. senal de aprendizaje por neurona (estilo e-prop).
    // Se difunde el error por la matriz de LECTURA, no por una matriz aleatoria.
    // Con B aleatoria cada neurona recibe el error de un simbolo al azar, asi
    // que la senal de tres factores no tiene credito especifico y solo inyecta
    // ruido: medido, empeoraba de forma monotona con eta. R_ si esta entrenada,
    // y su transpuesta es exactamente la direccion en que cada feature reduce
    // la perdida.
    //
    //   L_j = sum_c R[c][j] * error[c]
    //
    // R esta acotada a +-8, asi que hace falta un factor de escala: sin el, la
    // senal es ~8x mayor que con B normal y la plasticidad desestabiliza.
    {
        std::vector<float> fn;
        normalize(f, fn);
        std::fill(feedback_.begin(), feedback_.end(), 0.0f);
        const int stride = p_.n_features_max();
        const int F = std::min((int)fn.size(), stride);
        for (int j = 0; j < F; ++j) {
            float acc = 0.0f;
            for (int k = 0; k < p_.n_chars; ++k) acc += R_[(size_t)k * stride + j] * error[k];
            feedback_[j] = p_.eprop_scale * acc;
        }
        // El filtro paso bajo sigue haciendo falta: el error por simbolo es
        // casi todo ruido y la regla necesita una direccion estable.
        for (int j = 0; j < n_neurons_; ++j)
            feedback_[j] = p_.eta_slow * fb_prev_[j] + (1.0f - p_.eta_slow) * feedback_[j];
        fb_prev_ = feedback_;
    }

    // 2. regla de tres factores sobre la recurrencia.
    //    dw = -eta * error_difundido(post) * traza_pre(pre, retardo)
    // La traza se eleva al retardo de la sinapsis para alinearla con la llegada
    // del pulso: solo se refuerza la sinapsis que efectivamente contribuyo al
    // disparo postsinaptico. El signo negativo hace descender la perdida.
    for (auto& L : layers_) {
        const int n = L.n;
        for (int pre = 0; pre < n; ++pre) {
            const float tr = L.pre_trace[pre];
            if (tr < 1e-4f) continue;              // nada que aprender en esta sinapsis
            const i32 s0 = L.g.offsets[pre], s1 = L.g.offsets[pre + 1];
            for (i32 s = s0; s < s1; ++s) {
                const int post = L.g.post[s];
                const float fb = feedback_[L.offset + post];
                if (fb == 0.0f) continue;
                const float e = tr * std::exp(-(float)L.g.delay[s] / std::max(1.0f, p_.tau_elig));
                const float dw = -p_.eta * fb * e;
                const float w0 = L.g.w_init[s];
                float w = L.g.weight[s] + dw;
                const float lo = std::max(p_.w_min, w0 * (1.0f - p_.w_frac_max));
                const float hi = std::min(p_.w_max, w0 * (1.0f + p_.w_frac_max));
                L.g.weight[s] = std::max(lo, std::min(hi, w));
            }
        }
        // decay de la elegibilidad por simbolo (compatibilidad)
        for (auto& e : L.elig) { e *= p_.elig_decay; if (std::fabs(e) < 1e-6f) e = 0.0f; }
    }

    // 3. readout con Adam (sobre las features normalizadas)
    std::vector<float> fn;
    normalize(f, fn);
    const int F = (int)fn.size();
    const int stride = p_.n_features_max();
    // El sesgo tambien se acota. Sin esto diverge: Adam da pasos de tamano
    // unidad y sobre 1024 dimensiones el sesgo se va a +-455 en 100 pasos, con
    // lo que el softmax queda saturado y el BPC se dispara.
    for (int k = 0; k < p_.n_chars; ++k) {
        logits_b_[k] -= p_.readout_lr * error[k];
        if (logits_b_[k] > 8.0f) logits_b_[k] = 8.0f;
        else if (logits_b_[k] < -8.0f) logits_b_[k] = -8.0f;
    }
    static long step = 0;
    step++;
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    for (int k = 0; k < p_.n_chars; ++k) {
        const float g0 = error[k];
        if (g0 == 0.0f) continue;
        for (int i = 0; i < F; ++i) {
            size_t idx = (size_t)k * stride + i;
            float grad = g0 * fn[i];
            if (grad > p_.gclip) grad = p_.gclip;
            else if (grad < -p_.gclip) grad = -p_.gclip;
            R_m_[idx] = b1 * R_m_[idx] + (1 - b1) * grad;
            R_v_[idx] = b2 * R_v_[idx] + (1 - b2) * grad * grad;
            float mh = R_m_[idx] / (1 - std::pow(b1, (float)step));
            float vh = R_v_[idx] / (1 - std::pow(b2, (float)step));
            float nw = R_[idx] - p_.readout_lr * mh / (std::sqrt(vh) + eps);
            if (nw > 8.0f) nw = 8.0f;          // pesos acotados por elemento
            else if (nw < -8.0f) nw = -8.0f;
            R_[idx] = nw;
        }
    }

    // Proyeccion sobre una esfera por fila. Adam no converge a lr fijo cuando
    // las features derivan: los pesos crecen sin limite y el softmax se satura
    // (se midieron +-455 con 12800 simbolos). Renormalizar la fila conserva la
    // direccion del clasificador y acota su magnitud.
    if (p_.readout_norm > 0.0f)
    for (int k = 0; k < p_.n_chars; ++k) {
        float* row = &R_[(size_t)k * stride];
        float nrm = 0.0f;
        for (int i = 0; i < F; ++i) nrm += row[i] * row[i];
        nrm = std::sqrt(nrm);
        if (nrm > p_.readout_norm) {
            const float g = p_.readout_norm / nrm;
            for (int i = 0; i < F; ++i) row[i] *= g;
        }
    }
}

}  // namespace cortex
