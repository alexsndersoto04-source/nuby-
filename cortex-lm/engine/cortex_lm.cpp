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
#include <fstream>
#include <cctype>
#include <string>

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
        // Paso 4: traza postsinaptica.
        Lr.post_trace.assign(N, 0.0f);
        Lr.alpha_post = std::exp(-1.0f / std::max(1.0f, p_.tau_post));
        Lr.pre_trace_slow.assign(N, 0.0f);
        Lr.alpha_slow = std::exp(-1.0f / std::max(1.0f, p_.tau_elig * 6.0f));
        // Paso 5: prediccion top-down por neurona.
        Lr.pred.assign(N, 0.0f);
        Lr.pred_err.assign(N, 0.0f);

        queues_[li].init(p_.d_max);
    }

    // ------------------------------------------------------------------
    // JERARQUIA TEMPORAL: via lenta -> rapida (conectividad FIJA, no se entrena)
    // ------------------------------------------------------------------
    if (p_.n_fast > 0 && p_.n_fast < N) {
        const int nf = p_.n_fast;
        const int nslow = N - nf;
        const int fan = std::max(1, p_.slow_fanout);
        slow_src_.assign((size_t)nf * fan, 0);
        slow_w_.assign((size_t)nf * fan, 0.0f);
        std::uniform_int_distribution<int> sd_(0, nslow - 1);
        std::normal_distribution<float> gs(0.0f, 1.0f);
        const float sc = p_.slow_to_fast / std::sqrt((float)fan);
        for (int i = 0; i < nf; ++i)
            for (int k = 0; k < fan; ++k) {
                slow_src_[(size_t)i * fan + k] = nf + sd_(rng_);   // indice global
                slow_w_[(size_t)i * fan + k] = sc * (0.5f + std::fabs(gs(rng_)));
            }
        // Buffer circular por neurona rapida: guarda el caudal lento de los
        // ultimos `slow_delay` ticks. Es el retraso axonal largo.
        slow_buf_len_ = std::max(1, p_.slow_delay);
        slow_buf_.assign(nf, std::vector<float>(slow_buf_len_, 0.0f));
    } else {
        slow_buf_len_ = 0;
        slow_src_.clear();
        slow_w_.clear();
        slow_buf_.clear();
    }

    // ------------------------------------------------------------------
    // OSCILADOR INTERNO: marcapasos con periodo fijo y no multiplos
    // ------------------------------------------------------------------
    pace_phase_.assign(std::max(0, p_.n_pace), 0.0f);
    pace_period_.assign(std::max(0, p_.n_pace), 1.0f);
    if (p_.n_pace > 0) {
        const float base[4] = {p_.pace_period_a, p_.pace_period_b,
                               p_.pace_period_a * 2.7f, p_.pace_period_b * 1.7f};
        for (int i = 0; i < p_.n_pace; ++i)
            pace_period_[i] = std::max(2.0f, base[i % 4]);
        // fases repartidas para que no arranquen todos a la vez
        for (int i = 0; i < p_.n_pace; ++i)
            pace_phase_[i] = (float)i / (float)std::max(1, p_.n_pace);
    }

    dic_.clear();
    for (const auto& w : p_.diccionario) dic_.insert(w);
    dop_ = 0.0f;
    dop_evento_ = 0.0f;
    dop_pos_ = dop_neg_ = 0;
    palabra_.clear();

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
    // JERARQUIA: la poblacion lenta tambien recibe la entrada, pero mas debil
    // (slow_in_gain). Sin esto la memoria de trabajo se queda SORDA: sube su
    // membrana por la via rapida pero nunca sabe QUE se esta escribiendo, y su
    // estado no codifica nada. Medido: sin esta via la jerarquia no cambiaba el
    // texto en absoluto.
    if (p_.n_fast > 0 && p_.slow_in_gain > 0.0f) {
        const int nf = p_.n_fast;
        const int nslow = L0.n - nf;
        if (nslow > 0) {
            for (int i = 0; i < per; ++i) {
                const int j = nf + (dst[i] % nslow);
                L0.v[j] += p_.slow_in_gain * dw[i];
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
    for (int i = 0; i < mp; ++i) L0.v[dst[i]] += dw[i];
}

int Engine::step() {
    const float am = p_.alpha_m();
    int fired = 0;

    // Las trazas decaen una vez por tick, antes de la decision.
    for (auto& L : layers_) {
        for (auto& tr : L.pre_trace) tr *= L.alpha_elig;
        for (auto& tr : L.post_trace) tr *= L.alpha_post;   // Paso 4
        // traza lenta (via jerarquica): dura simbolos, no ticks
        for (auto& tr : L.pre_trace_slow) tr *= L.alpha_slow;
    }

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

        // 2. relajacion de la fatiga y de la membrana.
        //
        //    JERARQUIA TEMPORAL: la fuga NO es la misma para todas las neuronas.
        //    Las `n_fast` primeras usan exp(-1/tau_m_fast): olvidan en pocos
        //    ticks y siguen el ritmo de las letras. El resto usa
        //    exp(-1/tau_m_slow): su membrana apenas se descarga, asi que retiene
        //    el contexto de la frase mientras se escriben los caracteres
        //    siguientes. Es la memoria de trabajo que pide la pieza 2, y es
        //    puramente local: solo cambia una constante de tiempo por neurona.
        const float am_fast = p_.n_fast > 0
                            ? std::exp(-p_.dt / std::max(0.5f, p_.tau_m_fast)) : am;
        const float am_slow = p_.n_fast > 0
                            ? std::exp(-p_.dt / std::max(0.5f, p_.tau_m_slow)) : am;
        for (int j = 0; j < L.n; ++j) {
            L.theta[j] *= p_.theta_decay;
            if (L.theta[j] < 1e-4f) L.theta[j] = 0.0f;
            if (L.refr[j] > 0) { L.refr[j]--; continue; }
            const float amj = (p_.n_fast > 0 && j >= p_.n_fast) ? am_slow : am_fast;
            // La poblacion lenta tiene ademas un sesgo tonicamente mas alto: sin
            // el, con una fuga tan lenta nunca llega al umbral y se queda muda.
            const float extra = (p_.n_fast > 0 && j >= p_.n_fast) ? 0.0f : 0.0f;
            L.v[j] = (L.v[j] - p_.v_reset) * amj + p_.v_reset + L.bias[j] + extra
                     - i_inh;
            if (L.v[j] < 1e-4f) L.v[j] = p_.v_reset;
        }

        // 2b. OSCILADOR INTERNO: los marcapasos laten solos y su corriente entra
        //     como bias en la capa. Como su periodo no es multiplo del de otro,
        //     la fase relativa recorre muchos valores dentro de un bloque: eso
        //     es la "brujula" que le dice a la red en que punto va.
        if (!pace_phase_.empty()) {
            for (size_t i = 0; i < pace_phase_.size(); ++i) {
                pace_phase_[i] += 1.0f / pace_period_[i];
                if (pace_phase_[i] >= 1.0f) pace_phase_[i] -= 1.0f;
                // pulso gaussiano estrecho: dispara una vez por ciclo
                const float d = pace_phase_[i];
                const float pulso = std::exp(-0.5f * (d - 0.5f) * (d - 0.5f) / 0.0025f);
                const float amp = p_.pace_gain * pulso;
                if (amp < 1e-4f) continue;
                for (int j = 0; j < L.n; ++j) {
                    L.v[j] += amp * (j < p_.n_fast || p_.n_fast == 0
                                     ? p_.pace_bias_fast : p_.pace_bias_slow);
                }
            }
        }

        // 2c. VIA LENTA -> RAPIDA con retraso axonal largo.
        //     Cada neurona rapida guarda en su buffer circular la corriente que
        //     le manda la poblacion lenta; la lee `slow_delay` ticks despues.
        //     Es la realimentacion de la pieza 2: el estado lento (la frase)
        //     modula a la poblacion rapida mientras esta escribe las letras.
        if (!slow_buf_.empty() && (int)slow_buf_.size() <= L.n) {
            const int nf = (int)slow_buf_.size();
            const int fan = std::max(1, p_.slow_fanout);
            const int pos = (int)(t_ % slow_buf_len_);
            // escribir el caudal actual de la poblacion lenta
            for (int i = 0; i < nf; ++i) {
                float acc = 0.0f;
                for (int k = 0; k < fan; ++k) {
                    const int src = slow_src_[(size_t)i * fan + k];
                    if (src < L.n) acc += slow_w_[(size_t)i * fan + k] * L.v[src];
                }
                slow_buf_[i][pos] = acc;
            }
            // leer el que toca (retraso largo)
            for (int i = 0; i < nf; ++i)
                L.v[i] += slow_buf_[i][pos];
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
            L.pre_trace_slow[j] = 1.0f; // huella larga (via jerarquica)
            L.post_trace[j] = 1.0f;     // Paso 4: la de destino, en su capa
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
    // oscilador anclado: la fase vuelve al mismo punto en cada frontera de
    // simbolo. Cada marcapasos arranca desplazado, para que sus pulsos no
    // coincidan. Ver Params::pace_lock.
    if (p_.pace_lock && !pace_phase_.empty()) {
        const int np = (int)pace_phase_.size();
        for (int i = 0; i < np; ++i) {
            float f = p_.pace_phase_reset + (float)i / (float)np;
            if (f >= 1.0f) f -= 1.0f;
            pace_phase_[(size_t)i] = f;
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
    ch_actual_ = ch;                 // Paso 5: lo usa el error de prediccion
    inject_only(ch);
    for (int s = 0; s < p_.ticks_per_char; ++s) step();
    end_symbol();

    // ---- tercer factor: ¿lo que acaba de escribir se sostiene? ----
    // Se juzga con el diccionario y con los bigramas del corpus. El evento
    // MUEVE el nivel de dopamina; no lo fija. Asi el refuerzo tiene cola y
    // alcanza a las sinapsis que participaron en los ultimos ticks.
    const float ev = juzgar_palabra(ch);
    const float caida = std::exp(-(float)p_.ticks_per_char /
                                 std::max(1.0f, p_.dop_tau));
    dop_ *= caida;
    if (ev > 0.0f) {
        dop_ += p_.dop_rise * ev;
        dop_pos_++;
    } else if (ev < 0.0f) {
        dop_ += p_.dop_rise * ev;     // el castigo tambien tiene cola
        dop_neg_++;
    }
    if (dop_ > 1.0f) dop_ = 1.0f;
    if (dop_ < -1.0f) dop_ = -1.0f;

    // La dopamina tambien entra en la red como corriente moduladora: sube el
    // sesgo de toda la capa de forma difusa. Es lo que hace que el refuerzo se
    // note en el disparo y no solo en el peso.
    if (p_.dop_gain > 0.0f && dop_ != 0.0f)
        for (auto& L : layers_)
            for (int j = 0; j < L.n; ++j)
                L.v[j] += p_.dop_gain * 0.05f * dop_;
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


void Engine::state_features(std::vector<float>& out) const {
    // Potencial de membrana normalizado por el umbral. Es el estado real de la
    // red; la fatiga sola no lo es.
    const int F = p_.n_layers * p_.layer_size;
    out.assign(F, 0.0f);
    int idx = 0;
    const float inv = 1.0f / std::max(1e-3f, p_.v_th);
    for (const auto& L : layers_)
        for (int j = 0; j < L.n; ++j) out[idx++] = L.v[j] * inv;
}

void Engine::full_features(std::vector<float>& out) const {
    // estado (n_layer*N) + fatiga (n_layer*N), normalizados cada uno.
    const int N = p_.n_layers * p_.layer_size;
    std::vector<float> a, b;
    state_features(a);
    theta_features(b);
    out.assign((size_t)2 * N, 0.0f);
    for (int i = 0; i < N; ++i) out[i] = a[i];
    for (int i = 0; i < N; ++i) out[N + i] = b[i];
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

// ---------------------------------------------------------------------------
// PASO 5: senal de error propia de la capa
// ---------------------------------------------------------------------------
// La capa de entrada sabe que simbolo esta viendo (es su propia entrada). Su
// prediccion top-down es: "estas neuronas deberian haber disparado, porque son
// las que este simbolo enciende". El error de prediccion es la diferencia
// entre esa prediccion y los disparos que ocurrieron de verdad.
//
// Esta senal es LOCAL: cada neurona compara lo que esperaba de si misma con lo
// que hizo. No depende del lector, ni de una matriz aleatoria, ni de nadie
// aguas abajo. Y es verificable: se puede imprimir su magnitud.
void Engine::error_local(int capa, std::vector<float>& out) {
    if (capa < 0 || capa >= (int)layers_.size()) { out.clear(); return; }
    Layer& L = layers_[capa];
    out.assign(L.n, 0.0f);

    // poblacion que este simbolo deberia encender (la de su parche sensorial)
    std::vector<float> objetivo(L.n, 0.0f);
    if (capa == 0 && ch_actual_ >= 0 && ch_actual_ < p_.n_chars) {
        const int patch = p_.patch_size();
        const int tex = std::max(4, patch / 2);
        const int per = patch + tex;
        const int* dst = &sensory_[(size_t)ch_actual_ * per];
        for (int i = 0; i < per; ++i) objetivo[dst[i]] = 1.0f;
    }

    // disparos reales del ultimo simbolo, normalizados a [0,1]
    float maxr = 0.0f;
    for (int j = 0; j < L.n; ++j) maxr = std::max(maxr, L.rate[j]);
    const float inv = maxr > 1e-6f ? 1.0f / maxr : 0.0f;

    float suma = 0.0f, mx = 0.0f;
    for (int j = 0; j < L.n; ++j) {
        const float real = L.rate[j] * inv;
        // prediccion suavizada: sigue a la actividad esperada sin copiarla
        L.pred[j] += p_.pred_lr * (objetivo[j] - L.pred[j]);
        const float e = p_.pred_gain * (real - L.pred[j]);
        out[j] = e;
        suma += std::fabs(e);
        mx = std::max(mx, std::fabs(e));
    }
    L.pred_err = out;
    (void)suma; (void)mx;
}

void Engine::stats_error_local(int capa, float& medio, float& maximo) const {
    medio = 0.0f;
    maximo = 0.0f;
    if (capa < 0 || capa >= (int)layers_.size()) return;
    const Layer& L = layers_[capa];
    if (L.pred_err.empty()) return;
    double acc = 0.0;
    for (float e : L.pred_err) {
        acc += std::fabs(e);
        maximo = std::max(maximo, std::fabs(e));
    }
    medio = (float)(acc / (double)L.pred_err.size());
}


// ===========================================================================
// 1. MODULACION DOPAMINERGICA
// ===========================================================================
// La dopamina NO es un gradiente. Es una senal global, escalar, que no sabe que
// sinapsis tocaron ni que paso dentro de la red: solo "esto salio bien" o "esto
// salio mal". Esa falta de informacion es justo lo que la hace biologica: al
// llegar a todas las sinapsis a la vez, refuerza lo que ya estaba debilitandose
// (la traza de elegibilidad) sin poder dirigir el credito.
//
// Por eso la regla es  dw = eta * STDP * D(t)  y no  dw = eta * gradiente.
// La direccion la pone la STDP local; la dopamina solo decide SI se consolida.
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
    // Cuenta bigramas de letras del corpus y guarda solo los que aparecen lo
    // bastante para ser "estructura" y no casualidad. Un castigo por letra
    // imposible ("qzz") detecta ruido aunque la palabra entera sea nueva.
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
    for (auto& kv : cnt) {
        // probabilidad del bigrama, suavizada
        bigr_[kv.first] = (float)kv.second / (float)std::max(1, total);
    }
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
        // castigo inmediato por estructura imposible, sin esperar al final
        if (!bigr_.empty() && palabra_.size() >= 2 && dop_evento_ == 0.0f) {
            const uint32_t k = ((uint32_t)(unsigned char)palabra_[palabra_.size() - 2] << 8)
                             | (uint32_t)(unsigned char)palabra_.back();
            auto it = bigr_.find(k);
            const float p = (it == bigr_.end()) ? 0.0f : it->second;
            if (p < bigr_umbral_) dop_evento_ = -0.5f;   // letra que no sigue a la anterior
        }
        return dop_evento_;
    }

    // fin de palabra: se juzga
    if ((int)palabra_.size() >= p_.dop_min_len) {
        if (!dic_.empty() && dic_.count(palabra_) > 0)
            dop_evento_ = +1.0f;                        // palabra valida
        else
            dop_evento_ = -1.0f;                        // ruido
    }
    palabra_.clear();
    return dop_evento_;
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
    // Si no llega error, la senal local por neurona (Paso 5) es la unica que
    // se usa y este bloque no hace falta. Antes petaba con puntero nulo.
    if (error != nullptr && !R_.empty()) {
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

    // 2. REGLA DE TRES FACTORES (Pasos 4 y 5 del plan acordado).
    //
    //    dw = -eta * s_local(post) * traza_pre(pre) * traza_post(post)
    //
    //   - traza_pre(pre): el pulso presinaptico llego a tiempo de contribuir.
    //   - traza_post(post): la neurona de destino acaba de disparar. Es el
    //     factor que faltaba: sin el, la regla refuerza "lo que estaba activo
    //     cuando hubo error" en vez de "lo que hizo disparar a la neurona".
    //   - s_local(post): error de prediccion de la PROPIA capa (Paso 5), no el
    //     error del lector difundido por una matriz. Es local y verificable.
    //
    // El signo es negativo porque el error se define como (real - predicho):
    // si la neurona disparo mas de lo que predecía, el error es positivo y hay
    // que bajar el peso que la excito.
    //
    // La senal local por neurona se calcula UNA vez por capa y simbolo.
    std::vector<float> s_local;
    for (auto& L : layers_) {
        const int n = L.n;
        // Si el banco no rellena la senal local (capas sin prediccion), se cae
        // al error difundido de siempre, para no romper otras herramientas.
        error_local((int)(&L - layers_.data()), s_local);
        const bool usar_local = !s_local.empty();
        const float decay_post_w = p_.trace_post_w;

        for (int pre = 0; pre < n; ++pre) {
            const float tr = L.pre_trace[pre];
            if (tr < 1e-4f) continue;              // nada que aprender en esta sinapsis
            const i32 s0 = L.g.offsets[pre], s1 = L.g.offsets[pre + 1];
            for (i32 s = s0; s < s1; ++s) {
                const int post = L.g.post[s];
                const float tp = 1.0f + decay_post_w * (L.post_trace[post] - 1.0f);
                if (tp < 1e-4f) continue;          // el destino no disparo: no hubo causa
                const float sgn = usar_local ? s_local[post] : feedback_[L.offset + post];
                if (sgn == 0.0f) continue;
                const float e = tr * std::exp(-(float)L.g.delay[s] / std::max(1.0f, p_.tau_elig));
                // --- TERCER FACTOR (dopamina) ---
                // La Sinapsis no sabe si acerto. Solo sabe cuanto se movio
                // (eligibilidad e * tp). La dopamina decide si ese movimiento
                // se consolida: con D~0 no hay cambio de peso; con D=+1 se
                // consolida tal cual; con D=-1 se invierte el signo, que es lo
                // que hace un castigo (deshace lo que llevo al error).
                const float dop_factor = 1.0f + p_.dop_gain * dop_;
                const float dw = -p_.eta * sgn * e * tp * dop_factor;
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

    // 3. readout con Adam (sobre las features normalizadas).
    // Si no llega error del lector (senal puramente local, Paso 5), aqui no hay
    // nada que hacer: el lector de este banco es otro y aprende por su cuenta.
    if (error == nullptr) return;
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
