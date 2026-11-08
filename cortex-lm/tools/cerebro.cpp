// cerebro.cpp — Ciclo de vida completo para CORTEX-LM (4+ regiones corticales):
//   - Jerarquia cortical profunda L0 -> L1 -> L2 <-> L3 con dendritas apicales Larkum
//   - Trazas lentas NMDA multi-region + retencion conversacional (carry_context)
//   - Deliberacion fisica con espigas ("pensar antes de hablar": ? [[...]] -> > ...)
//   - Subregion hipocampal CA3 pura (engramas Hebbianos en 1 disparo + replay local)
//   - Aprendizaje continuo con consolidacion sinaptica elastica
//
// 100% neuronas spiking LIF puras en C++ + reglas locales. Cero backpropagation.
//
// Comandos:
//   cerebro entrenar    <modelo.bin> <corpus.txt> [--capas 1024,10240,10240,8192]
//                       [--grado 48] [--epocas 4] [--lr 0.45] [--stdp 1]
//                       [--holdout 0.10] [--max-chars 0] [--seed 1234]
//                       [--continuo] [--ancla 0.03]
//   cerebro alinear     <modelo.bin> <alineacion.txt> [--epocas 24] [--lr 0.24] [--ancla 0.03]
//   cerebro recordar    <modelo.bin> "<hecho o recuerdo nuevo>"
//   cerebro evaluar     <modelo.bin> <holdout.txt> [dic.txt]
//   cerebro responder   <modelo.bin> "<pregunta>" [--pensar] [--directo]
//                       [--prev "<pregunta_turno_1>"] [--temp 0.15] [--bytes 280] [--crudo]
//   cerebro interactivo <modelo.bin> [--temp 0.20] [--pensar]

#include "cortex_lm.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace cortex;

static constexpr uint32_t kMagicBrain  = 0x4E554259u; // "NUBY"
static constexpr uint32_t kMagicDorsal = 0x43584431u; // "CXD1"
static constexpr uint32_t kMagicHipp   = 0x43584832u; // "CXH2" (CA3 + lector CA1)
static constexpr uint32_t kMagicReplay = 0x43584231u; // "CXB1" (banco replay sueno dorsal)

static std::string leer_archivo(const std::string& ruta) {
    std::ifstream f(ruta, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "Error: no se pudo abrir '%s'\n", ruta.c_str());
        std::exit(1);
    }
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

static bool existe_archivo(const std::string& ruta) {
    std::ifstream f(ruta, std::ios::binary);
    return (bool)f;
}

static std::unordered_set<std::string> cargar_dic(const std::string& ruta) {
    std::unordered_set<std::string> d;
    if (ruta.empty()) return d;
    std::ifstream f(ruta);
    std::string w;
    while (f >> w) d.insert(w);
    return d;
}

static std::string minuscula_utf8(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        uint8_t c = (uint8_t)s[i];
        if (c >= 'A' && c <= 'Z') r.push_back((char)(c + 32));
        else if (c == 0xC3 && i + 1 < s.size()) {
            uint8_t d = (uint8_t)s[i + 1];
            if (d >= 0x80 && d <= 0x9E) d += 0x20;
            r.push_back((char)c);
            r.push_back((char)d);
            ++i;
        } else r.push_back((char)c);
    }
    return r;
}

static bool es_letra_es(const std::string& s, size_t i, size_t& adv) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) { adv = 1; return true; }
    if (c == 0xC3 && i + 1 < s.size()) {
        uint8_t d = (uint8_t)s[i + 1];
        if ((d >= 0x80 && d <= 0x9E) || (d >= 0xA0 && d <= 0xBE)) { adv = 2; return true; }
    }
    adv = 1;
    return false;
}

static std::pair<int, int> contar_palabras_reales(
    const std::string& texto, const std::unordered_set<std::string>& dic) {
    int tot = 0, ok = 0;
    size_t i = 0;
    while (i < texto.size()) {
        size_t adv = 1;
        if (!es_letra_es(texto, i, adv)) { i += adv; continue; }
        size_t j = i;
        int n_letras = 0;
        while (j < texto.size()) {
            size_t a2 = 1;
            if (!es_letra_es(texto, j, a2)) break;
            j += a2;
            n_letras++;
        }
        if (n_letras >= 3) {
            tot++;
            if (dic.count(minuscula_utf8(texto.substr(i, j - i)))) ok++;
        }
        i = j;
    }
    return {ok, tot};
}

static bool utf8_transicion_valida(uint8_t prev_lead_rem, uint8_t cand) {
    const bool is_cont = (cand >= 0x80 && cand <= 0xBF);
    if (prev_lead_rem > 0) return is_cont;
    if (is_cont) return false;
    return true;
}

static int utf8_rem_after(uint8_t prev_lead_rem, uint8_t b) {
    if (prev_lead_rem > 0) return (int)prev_lead_rem - 1;
    if ((b & 0xE0) == 0xC0) return 1;
    if ((b & 0xF0) == 0xE0) return 2;
    if ((b & 0xF8) == 0xF0) return 3;
    return 0;
}

static void limpiar_utf8_final(std::string& s) {
    while (!s.empty() && (((uint8_t)s.back() & 0xC0) == 0xC0)) {
        s.pop_back();
    }
}

static std::vector<int> parsear_capas(const std::string& csv) {
    std::vector<int> v;
    std::stringstream ss(csv);
    std::string it;
    while (std::getline(ss, it, ',')) {
        if (!it.empty()) v.push_back(std::stoi(it));
    }
    if (v.empty()) v = {1024, 10240, 10240, 8192};
    return v;
}

struct EstadoCerebro {
    Engine eng;
    CorticalReadout rd_ventral;
    CorticalReadout rd_dorsal;
    EpisodicCA3 ca3;
    CorticalReadout rd_ca1;
    std::vector<std::string> dorsal_replay;
    std::vector<float> wm_acc;
    bool tiene_dorsal = false;
    bool tiene_ca3 = false;
};

static void guardar_cerebro_completo(const std::string& ruta, const EstadoCerebro& st) {
    save_brain(ruta, st.eng, st.rd_ventral);
    std::ofstream out(ruta, std::ios::binary | std::ios::app);
    if (st.tiene_dorsal) {
        out.write(reinterpret_cast<const char*>(&kMagicDorsal), sizeof(kMagicDorsal));
        st.rd_dorsal.save(out);
    }
    if (st.tiene_ca3 && st.ca3.num_engrams() > 0) {
        out.write(reinterpret_cast<const char*>(&kMagicHipp), sizeof(kMagicHipp));
        st.ca3.save(out);
        st.rd_ca1.save(out);
    }
    if (!st.dorsal_replay.empty()) {
        out.write(reinterpret_cast<const char*>(&kMagicReplay), sizeof(kMagicReplay));
        const uint32_t nb = (uint32_t)st.dorsal_replay.size();
        out.write(reinterpret_cast<const char*>(&nb), sizeof(nb));
        for (const std::string& s : st.dorsal_replay) {
            const uint32_t len = (uint32_t)s.size();
            out.write(reinterpret_cast<const char*>(&len), sizeof(len));
            if (len > 0) out.write(s.data(), len);
        }
    }
}

static void cargar_cerebro_completo(const std::string& ruta, EstadoCerebro& st) {
    std::ifstream in(ruta, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "Error: no se puede abrir el modelo '%s'\n", ruta.c_str());
        std::exit(1);
    }
    uint32_t magic = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != kMagicBrain) {
        std::fprintf(stderr, "Error: '%s' no es un cerebro CORTEX-LM valido\n", ruta.c_str());
        std::exit(1);
    }
    st.eng.load_engine(in);
    st.rd_ventral.load(in);
    st.tiene_dorsal = false;
    st.tiene_ca3 = false;
    st.dorsal_replay.clear();
    st.ca3.init(st.eng.nmda_concept_dim());

    uint32_t tag = 0;
    while (in.read(reinterpret_cast<char*>(&tag), sizeof(tag))) {
        if (tag == kMagicDorsal) {
            st.rd_dorsal.load(in);
            st.tiene_dorsal = true;
        } else if (tag == kMagicHipp) {
            st.ca3.load(in);
            st.rd_ca1.load(in);
            st.tiene_ca3 = true;
        } else if (tag == kMagicReplay) {
            uint32_t nb = 0;
            in.read(reinterpret_cast<char*>(&nb), sizeof(nb));
            st.dorsal_replay.assign(nb, {});
            for (uint32_t k = 0; k < nb; ++k) {
                uint32_t len = 0;
                in.read(reinterpret_cast<char*>(&len), sizeof(len));
                st.dorsal_replay[k].resize(len);
                if (len > 0) in.read(&st.dorsal_replay[k][0], len);
            }
        } else {
            break;
        }
    }
}

static void agregar_al_banco_dorsal(std::vector<std::string>& banco,
                                    const std::vector<std::string>& nuevos) {
    std::unordered_set<std::string> vistos(banco.begin(), banco.end());
    for (const std::string& b : nuevos) {
        if (!vistos.count(b)) {
            vistos.insert(b);
            banco.push_back(b);
        }
    }
}

static uint8_t muestrear_byte_utf8(const std::vector<double>& probs_raw,
                                   const std::string& hist,
                                   uint8_t u8_rem,
                                   double temp,
                                   std::mt19937& rng) {
    std::vector<double> probs = probs_raw;
    if (hist.size() >= 6) {
        const std::string ctx5 = hist.substr(hist.size() - 5);
        size_t pos = 0;
        while ((pos = hist.find(ctx5, pos)) != std::string::npos && pos + 5 < hist.size() - 1) {
            uint8_t prev_next = (uint8_t)hist[pos + 5];
            probs[prev_next] *= 0.35;
            pos++;
        }
    }

    uint8_t pick = ' ';
    if (temp <= 0.12) {
        double best_p = -1.0;
        for (int b = 0; b < 256; ++b) {
            if (!utf8_transicion_valida(u8_rem, (uint8_t)b)) continue;
            if (probs[(size_t)b] > best_p) {
                best_p = probs[(size_t)b];
                pick = (uint8_t)b;
            }
        }
    } else {
        double sum = 0.0;
        for (int b = 0; b < 256; ++b) {
            if (!utf8_transicion_valida(u8_rem, (uint8_t)b) || probs[(size_t)b] < 1e-4) {
                probs[(size_t)b] = 0.0;
            }
            sum += probs[(size_t)b];
        }
        if (sum > 0.0) {
            std::uniform_real_distribution<double> ud(0.0, sum);
            double r = ud(rng), acc = 0.0;
            for (int b = 0; b < 256; ++b) {
                acc += probs[(size_t)b];
                if (r <= acc) { pick = (uint8_t)b; break; }
            }
        }
    }
    return pick;
}

static std::string generar_prosa_libre(Engine& eng,
                                       const CorticalReadout& rd,
                                       const std::string& entrada,
                                       int max_bytes,
                                       double temp,
                                       uint32_t seed_gen = 777,
                                       float nmda_gain = 0.62f) {
    eng.reset_state();
    eng.set_plasticity(false);
    eng.set_executive_mode(false);
    uint8_t u8_rem = 0;
    for (unsigned char b : entrada) {
        eng.feed(b);
        u8_rem = (uint8_t)utf8_rem_after(u8_rem, b);
    }

    std::vector<std::pair<i32, float>> feats;
    std::vector<double> probs(256);
    std::mt19937 rng(seed_gen);
    std::string out;

    for (int g = 0; g < max_bytes; ++g) {
        eng.cortical_sparse_features(feats, 2, nmda_gain);
        rd.predict(feats, probs.data(), std::max(0.08, temp));
        uint8_t pick = muestrear_byte_utf8(probs, out, u8_rem, temp, rng);
        out.push_back((char)pick);
        u8_rem = (uint8_t)utf8_rem_after(u8_rem, pick);
        eng.feed(pick);
    }
    limpiar_utf8_final(out);
    return out;
}

struct SalidaDeliberativa {
    std::string recuerdo_ca3;
    float       sim_ca3 = 0.0f;
    std::string pensamiento;
    std::string respuesta;
};

// Ejecuta el ciclo cognitivo completo 100% spiking LIF:
//   1) Retencion opcional de contexto NMDA entre turnos (carry_context)
//   2) Alimentacion de la pregunta y consulta sinaptica hipocampal CA3 (SDR NMDA)
//   3) Si usar_deliberacion=true: generacion fisica de monologo interno ? [[...]]
//      cuyas espigas realimentan L0->L1->L2<->L3 antes de emitir la respuesta audible
//   4) Generacion de la respuesta audible tras "\n> "
static SalidaDeliberativa responder_cognitivo(EstadoCerebro& st,
                                              const std::string& pregunta,
                                              int max_bytes,
                                              double temp,
                                              bool usar_deliberacion,
                                              bool conservar_contexto,
                                              uint32_t seed_gen = 2026) {
    SalidaDeliberativa res;
    Engine& eng = st.eng;

    if (conservar_contexto) {
        eng.carry_context(0.38f);
    } else {
        eng.reset_state();
        st.wm_acc.clear();
    }
    eng.set_plasticity(false);

    const bool tiene_ca3_activo = (st.tiene_ca3 && st.ca3.num_engrams() > 0);
    std::vector<float> cur_wm;
    std::vector<float> epi_acc;
    uint8_t u8_rem = 0;
    for (unsigned char b : pregunta) {
        eng.feed(b);
        eng.accumulate_episodic_sdr(cur_wm);
        if (tiene_ca3_activo) eng.accumulate_episodic_sdr(epi_acc);
        u8_rem = (uint8_t)utf8_rem_after(u8_rem, b);
    }
    // Normalizar L2 la memoria de trabajo de la pregunta actual para que preguntas largas
    // de turnos viejos nunca dominen sobre preguntas cortas del turno inmediatamente previo:
    double nsq = 0.0;
    for (float v : cur_wm) nsq += (double)v * (double)v;
    if (nsq > 1e-12) {
        const float inv = (float)(1.0 / std::sqrt(nsq));
        for (float& v : cur_wm) v *= inv;
    }
    std::vector<float> mix_wm = cur_wm;
    if (conservar_contexto && st.wm_acc.size() == mix_wm.size()) {
        for (size_t k = 0; k < mix_wm.size(); ++k) mix_wm[k] += 0.42f * st.wm_acc[k];
    }
    st.wm_acc = cur_wm;

    std::vector<std::pair<i32, float>> q_wm;
    eng.finalize_episodic_sdr(mix_wm, q_wm, 192);
    const i32 base_nmda = eng.cortical_feature_dim(2) - eng.nmda_concept_dim();

    // Consulta asociativa a la subregion hipocampal CA3 usando el SDR entorrinal-NMDA
    // acumulado durante toda la pregunta (sim >= 0.20 separa nitidamente recuerdos reales
    // ~0.28..0.60 del solapamiento fortuito de palabras funcionales ~0.04..0.11).
    bool usar_lector_ca1 = false;
    if (tiene_ca3_activo) {
        std::vector<std::pair<i32, float>> q_sdr;
        eng.finalize_episodic_sdr(epi_acc, q_sdr, 256);
        std::string rec;
        float sim = 0.0f;
        if (st.ca3.recall(q_sdr, rec, 0.20f, &sim)) {
            res.recuerdo_ca3 = rec;
            res.sim_ca3 = sim;
            usar_lector_ca1 = true;
            const std::string tag_mem = "\n[CA3: " + rec + "]";
            for (unsigned char b : tag_mem) {
                eng.feed(b);
                u8_rem = (uint8_t)utf8_rem_after(u8_rem, b);
            }
        }
    }

    const CorticalReadout& rd = usar_lector_ca1
                              ? st.rd_ca1
                              : (st.tiene_dorsal ? st.rd_dorsal : st.rd_ventral);
    const float gain_nmda = (usar_lector_ca1 || st.tiene_dorsal) ? 2.85f : 0.62f;

    std::vector<std::pair<i32, float>> feats;
    std::vector<double> probs(256);
    std::mt19937 rng(seed_gen);

    if (usar_deliberacion && (st.tiene_dorsal || usar_lector_ca1)) {
        const std::string pref_think = "\n~ ";
        for (unsigned char b : pref_think) {
            eng.feed(b);
            u8_rem = (uint8_t)utf8_rem_after(u8_rem, b);
        }

        std::string think_buf;
        bool cerro_pensamiento = false;
        const double temp_think = std::min(temp, 0.12);
        for (int g = 0; g < 160; ++g) {
            eng.cortical_sparse_features(feats, 2, gain_nmda);
            if (!usar_lector_ca1 && st.tiene_dorsal) {
                const float g_wm = 2.90f * std::exp(-(float)g / 12.0f);
                if (g_wm > 0.12f) {
                    for (const auto& kv : q_wm) feats.push_back({base_nmda + kv.first, kv.second * g_wm});
                }
            }
            rd.predict(feats, probs.data(), std::max(0.08, temp_think));
            uint8_t pick = muestrear_byte_utf8(probs, think_buf, u8_rem, temp_think, rng);
            think_buf.push_back((char)pick);
            u8_rem = (uint8_t)utf8_rem_after(u8_rem, pick);
            eng.feed(pick);

            if (u8_rem == 0 && think_buf.back() == '\n') {
                res.pensamiento = think_buf.substr(0, think_buf.size() - 1);
                eng.feed((uint8_t)'>');
                eng.feed((uint8_t)' ');
                cerro_pensamiento = true;
                break;
            }
        }
        if (!cerro_pensamiento) {
            limpiar_utf8_final(think_buf);
            res.pensamiento = think_buf;
            const std::string cierre = "\n> ";
            u8_rem = 0;
            for (unsigned char b : cierre) eng.feed(b);
        }
    } else {
        const std::string pref_ans = "\n> ";
        for (unsigned char b : pref_ans) {
            eng.feed(b);
            u8_rem = (uint8_t)utf8_rem_after(u8_rem, b);
        }
    }

    // Generar respuesta audible condicionada por las asambleas L2/L3 encendidas
    std::string out;
    for (int g = 0; g < max_bytes; ++g) {
        eng.cortical_sparse_features(feats, 2, gain_nmda);
        if (!usar_lector_ca1 && st.tiene_dorsal) {
            const float g_wm = 2.90f * std::exp(-(float)g / 12.0f);
            if (g_wm > 0.12f) {
                for (const auto& kv : q_wm) feats.push_back({base_nmda + kv.first, kv.second * g_wm});
            }
        }
        rd.predict(feats, probs.data(), std::max(0.08, temp));
        uint8_t pick = muestrear_byte_utf8(probs, out, u8_rem, temp, rng);
        out.push_back((char)pick);
        u8_rem = (uint8_t)utf8_rem_after(u8_rem, pick);
        eng.feed(pick);

        if (u8_rem == 0) {
            if (out.size() >= 2 && out.substr(out.size() - 2) == "\n\n") {
                out.resize(out.size() - 2);
                break;
            }
            if (!out.empty() && out.back() == '\n') {
                out.pop_back();
                break;
            }
        }
    }
    limpiar_utf8_final(out);
    res.respuesta = out;
    return res;
}

static double evaluar_bpc_texto(Engine& eng, CorticalReadout& rd,
                                const std::string& txt, size_t offset_eval = 0) {
    eng.reset_state();
    eng.set_plasticity(false);
    eng.set_executive_mode(false);
    std::vector<std::pair<i32, float>> feats;

    const size_t warm_start = (offset_eval > 64) ? (offset_eval - 64) : 0;
    for (size_t i = warm_start; i < offset_eval && i < txt.size(); ++i) {
        eng.feed((uint8_t)txt[i]);
    }
    double bits = 0.0;
    size_t cnt = 0;
    for (size_t i = offset_eval; i + 1 < txt.size(); ++i) {
        eng.feed((uint8_t)txt[i]);
        if (offset_eval > 0 || i >= 32) {
            eng.cortical_sparse_features(feats, 2);
            bits += rd.step((uint8_t)txt[i + 1], feats, false, 0.0f);
            cnt++;
        }
    }
    return cnt ? (bits / (double)cnt) : 0.0;
}

// Memorizacion episodica en 1 disparo (CA3 Hebbiano + consolidacion local en CA1)
static void memorizar_episodio_ca3(EstadoCerebro& st, const std::string& hecho) {
    if (hecho.empty()) return;
    if (!st.tiene_ca3 || st.ca3.num_engrams() == 0) {
        st.ca3.init(st.eng.nmda_concept_dim());
        st.rd_ca1 = st.tiene_dorsal ? st.rd_dorsal : st.rd_ventral;
        st.rd_ca1.scale_weights(0.25f);
    }

    // 1. Extraer SDR entorrinal-NMDA (L2/L3) a lo largo de todo el nuevo episodio
    st.eng.reset_state();
    st.eng.set_plasticity(false);
    st.eng.set_executive_mode(true);
    std::vector<float> epi_acc;
    for (unsigned char b : hecho) {
        st.eng.feed(b);
        st.eng.accumulate_episodic_sdr(epi_acc);
    }
    std::vector<std::pair<i32, float>> sdr;
    st.eng.finalize_episodic_sdr(epi_acc, sdr, 256);
    st.ca3.memorize(sdr, hecho);
    st.tiene_ca3 = true;

    // 2. Replay hipocampal entrelazado (ondas agudas CA3 -> CA1):
    //    reproduce de forma entrelazada los engramas activos de CA3 para que CA1
    //    separe limpiamente cada recuerdo sin sobreescribir los anteriores ni tocar rd_dorsal.
    st.rd_ca1 = st.tiene_dorsal ? st.rd_dorsal : st.rd_ventral;
    st.rd_ca1.scale_weights(0.20f);

    std::vector<std::string> replay_blks;
    const size_t n_eng = st.ca3.num_engrams();
    const size_t start_e = (n_eng > 16) ? (n_eng - 16) : 0;
    for (size_t ei = start_e; ei < n_eng; ++ei) {
        const std::string& h = st.ca3.engram(ei);
        replay_blks.push_back(h + "\n[CA3: " + h + "]\n~ memoria CA3 -> " + h +
                              "\n> Según recuerdo: " + h + "\n\n");
        replay_blks.push_back("¿Qué sabes de eso?\n[CA3: " + h + "]\n~ memoria CA3 -> " + h +
                              "\n> Según recuerdo: " + h + "\n\n");
    }
    for (const std::string& blk : replay_blks) {
        for (unsigned char b : blk) st.rd_ca1.ensure_byte_slot(b);
    }

    std::vector<std::pair<i32, float>> feats;
    const std::string tag_th = "\n~ ";
    const std::string tag_an = "\n> ";
    std::mt19937 rrng(1337);
    std::vector<size_t> ord(replay_blks.size());
    for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;

    for (int rep = 0; rep < 14; ++rep) {
        std::shuffle(ord.begin(), ord.end(), rrng);
        st.rd_ca1.set_lr(0.42f / (1.0f + 0.06f * rep));
        for (size_t idx : ord) {
            const std::string& blk = replay_blks[idx];
            const size_t p_th = blk.find(tag_th);
            const size_t p_an = blk.find(tag_an);
            const size_t ini = (p_th != std::string::npos) ? (p_th + tag_th.size()) : 0;
            const size_t ini_an = (p_an != std::string::npos) ? (p_an + tag_an.size()) : blk.size();

            st.eng.reset_state();
            for (size_t i = 0; i + 1 < blk.size(); ++i) {
                st.eng.feed((uint8_t)blk[i]);
                if (i + 1 >= ini) {
                    st.eng.cortical_sparse_features(feats, 2, 2.85f);
                    const bool arranque = ((i + 1 - ini) < 12) ||
                                          (i + 1 >= ini_an && (i + 1 - ini_an) < 18);
                    st.rd_ca1.step((uint8_t)blk[i + 1], feats, true, arranque ? 3.5f : 1.0f);
                }
            }
        }
    }
}

// ============================================================================
// COMANDO 1: entrenar (inicial o continuo sobre un cerebro ya guardado)
// ============================================================================
static int cmd_entrenar(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "Uso: %s entrenar <modelo.bin> <corpus.txt> [opciones]\n", argv[0]);
        return 1;
    }
    const std::string ruta_modelo = argv[2];
    const std::string ruta_corpus = argv[3];

    std::string capas_str = "1024,10240,10240,8192";
    int grado = 48;
    int epocas = 4;
    float lr = 0.45f;
    int stdp = 1;
    double holdout_frac = 0.15;
    size_t max_chars = 0;
    uint32_t seed = 1234;
    bool continuo = false;
    float ancla = 0.03f;

    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--capas" && i + 1 < argc) capas_str = argv[++i];
        else if (a == "--grado" && i + 1 < argc) grado = std::atoi(argv[++i]);
        else if (a == "--epocas" && i + 1 < argc) epocas = std::atoi(argv[++i]);
        else if (a == "--lr" && i + 1 < argc) lr = std::strtof(argv[++i], nullptr);
        else if (a == "--stdp" && i + 1 < argc) stdp = std::atoi(argv[++i]);
        else if (a == "--holdout" && i + 1 < argc) holdout_frac = std::strtod(argv[++i], nullptr);
        else if (a == "--max-chars" && i + 1 < argc) max_chars = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--seed" && i + 1 < argc) seed = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--continuo") continuo = true;
        else if (a == "--ancla" && i + 1 < argc) ancla = std::strtof(argv[++i], nullptr);
    }

    std::string data = leer_archivo(ruta_corpus);
    if (max_chars > 0 && data.size() > max_chars) {
        size_t cut = max_chars;
        while (cut < data.size() && ((uint8_t)data[cut] & 0xC0) == 0x80) cut++;
        data.resize(cut);
    }
    const size_t N = data.size();
    const size_t NTR = (holdout_frac > 0.001 && holdout_frac < 0.5)
                       ? (size_t)(N * (1.0 - holdout_frac))
                       : N;

    EstadoCerebro st;
    std::vector<std::pair<i32, float>> feats;
    auto t_global = std::chrono::steady_clock::now();

    if (continuo) {
        if (!existe_archivo(ruta_modelo)) {
            std::fprintf(stderr, "Error: --continuo requiere que exista '%s'\n", ruta_modelo.c_str());
            return 1;
        }
        cargar_cerebro_completo(ruta_modelo, st);
        st.rd_ventral.consolidate(ancla);
        int antes = st.rd_ventral.active_vocab_size();
        for (size_t i = 0; i < NTR; ++i) st.rd_ventral.ensure_byte_slot((uint8_t)data[i]);
        int despues = st.rd_ventral.active_vocab_size();
        std::printf("[CONTINUO] Cargado '%s' | %d regiones | %d neuronas | %zu sinapsis | vocab %d->%d bytes | ancla=%.3f\n",
                    ruta_modelo.c_str(), st.eng.num_layers(), st.eng.n_neurons(), st.eng.n_synapses(),
                    antes, despues, ancla);
    } else {
        Params p;
        p.layer_sizes = parsear_capas(capas_str);
        p.cortical_hierarchy = (p.layer_sizes.size() >= 2);
        p.max_out_degree = grado;
        p.ticks_per_char = 8;
        if (!stdp) {
            p.stdp_ltp = 0.0f;
            p.stdp_ltd = 0.0f;
            p.homeo_thr_up = 0.0f;
            p.homeo_thr_down = 0.0f;
        }
        st.eng = Engine(p);
        st.eng.build_random(seed);
        st.rd_ventral.init(st.eng.cortical_feature_dim(2), lr, 0.0f);
        st.ca3.init(st.eng.nmda_concept_dim());
        for (size_t i = 0; i < NTR; ++i) st.rd_ventral.ensure_byte_slot((uint8_t)data[i]);

        std::printf("[NUEVO] Cerebro LIF puro %s (%d regiones%s) | %d neuronas | %zu sinapsis CSR | train=%zu chars | holdout=%zu chars\n",
                    capas_str.c_str(), st.eng.num_layers(),
                    st.eng.num_layers() >= 4 ? " + penacho apical Larkum" : "",
                    st.eng.n_neurons(), st.eng.n_synapses(), NTR, N - NTR);
        std::fflush(stdout);

        if (stdp) {
            st.eng.reset_state();
            st.eng.set_plasticity(true);
            const size_t N_CRIT = std::min<size_t>(NTR, 65000);
            for (size_t i = 0; i < N_CRIT; ++i) st.eng.feed((uint8_t)data[i]);
            std::printf("  Periodo critico STDP completado (%zu simbolos no supervisados)\n", N_CRIT);
            std::fflush(stdout);
        }

        st.eng.reset_state();
        st.eng.set_plasticity(false);
        const size_t NCAL = std::min<size_t>(3500, NTR);
        for (size_t i = 0; i < NCAL; ++i) {
            st.eng.feed((uint8_t)data[i]);
            st.eng.cortical_sparse_features(feats, 2);
            st.rd_ventral.calibrate_step(feats);
        }
        st.rd_ventral.freeze_calibration();
    }

    for (int ep = 0; ep < epocas; ++ep) {
        auto t_ep = std::chrono::steady_clock::now();
        st.eng.reset_state();
        st.eng.set_plasticity(false);
        const float cur_lr = lr / (1.0f + 0.12f * ep);
        st.rd_ventral.set_lr(cur_lr);

        double bits = 0.0;
        size_t cnt = 0;
        for (size_t i = 0; i + 1 < NTR; ++i) {
            st.eng.feed((uint8_t)data[i]);
            if (i >= 32) {
                st.eng.cortical_sparse_features(feats, 2);
                bits += st.rd_ventral.step((uint8_t)data[i + 1], feats, true, 1.0f);
                cnt++;
            }
        }
        const double tr_bpc = bits / (double)std::max<size_t>(1, cnt);
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_ep).count();

        if (N > NTR + 64) {
            const double ho_bpc = evaluar_bpc_texto(st.eng, st.rd_ventral, data, NTR);
            std::printf("  epoca %2d/%d (%5.1fs, lr=%.3f): train=%.3f bpc | HOLDOUT=%.3f bpc\n",
                        ep + 1, epocas, dt, cur_lr, tr_bpc, ho_bpc);
        } else {
            std::printf("  epoca %2d/%d (%5.1fs, lr=%.3f): train=%.3f bpc\n",
                        ep + 1, epocas, dt, cur_lr, tr_bpc);
        }
        guardar_cerebro_completo(ruta_modelo, st);
        std::fflush(stdout);
    }

    const double tot_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_global).count();
    std::printf("[GUARDADO] '%s' listo en %.1fs.\n", ruta_modelo.c_str(), tot_s);
    return 0;
}

// ============================================================================
// COMANDO 2: alinear (via dorsal deliberativa + multi-turno carry_context)
// ============================================================================
static int cmd_alinear(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "Uso: %s alinear <modelo.bin> <alineacion.txt> [--epocas 24] [--lr 0.24] [--ancla 0.03]\n", argv[0]);
        return 1;
    }
    const std::string ruta_modelo = argv[2];
    const std::string ruta_ali = argv[3];
    int epocas = 24;
    float lr = 0.24f;
    float ancla = 0.03f;
    bool continuo_dorsal = false;

    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--epocas" && i + 1 < argc) epocas = std::atoi(argv[++i]);
        else if (a == "--lr" && i + 1 < argc) lr = std::strtof(argv[++i], nullptr);
        else if (a == "--ancla" && i + 1 < argc) ancla = std::strtof(argv[++i], nullptr);
        else if (a == "--continuo") continuo_dorsal = true;
    }

    const std::string txt = leer_archivo(ruta_ali);
    EstadoCerebro st;
    cargar_cerebro_completo(ruta_modelo, st);
    if (!st.tiene_dorsal || !continuo_dorsal) {
        st.rd_dorsal = st.rd_ventral;
        st.rd_dorsal.scale_weights(0.20f);
        st.tiene_dorsal = true;
    }
    st.rd_dorsal.consolidate(ancla);
    for (unsigned char b : txt) st.rd_dorsal.ensure_byte_slot(b);

    std::vector<std::string> bloques;
    {
        size_t p0 = 0;
        while (p0 < txt.size()) {
            while (p0 < txt.size() && (txt[p0] == '\n' || txt[p0] == '\r' || txt[p0] == ' ')) p0++;
            if (p0 >= txt.size()) break;
            size_t p1 = txt.find("\n\n", p0);
            std::string blk = (p1 == std::string::npos) ? txt.substr(p0) : txt.substr(p0, p1 - p0);
            while (!blk.empty() && (blk.back() == '\n' || blk.back() == '\r')) blk.pop_back();
            if (!blk.empty()) bloques.push_back(blk + "\n\n");
            if (p1 == std::string::npos) break;
            p0 = p1 + 2;
        }
    }

    if (!continuo_dorsal) st.dorsal_replay.clear();
    agregar_al_banco_dorsal(st.dorsal_replay, bloques);
    if (continuo_dorsal) bloques = st.dorsal_replay;
    for (const std::string& b : bloques) {
        for (unsigned char ch : b) st.rd_dorsal.ensure_byte_slot(ch);
    }

    std::printf("[ALINEAR VIA DORSAL DELIBERATIVA] '%s' sobre %zu episodios (%zu chars) | epocas=%d | lr=%.3f | ancla=%.3f\n",
                ruta_modelo.c_str(), bloques.size(), txt.size(), epocas, lr, ancla);

    std::vector<std::pair<i32, float>> feats;
    const std::string sep_turno = "\n---\n";
    const std::string tag_th = "\n~ ";
    const std::string tag_an = "\n> ";
    std::mt19937 srng(2026);
    std::vector<size_t> orden(bloques.size());
    for (size_t i = 0; i < orden.size(); ++i) orden[i] = i;

    const i32 base_nmda = st.eng.cortical_feature_dim(2) - st.eng.nmda_concept_dim();
    std::vector<float> wm_acc;
    std::vector<std::pair<i32, float>> q_wm;

    for (int ep = 0; ep < epocas; ++ep) {
        std::shuffle(orden.begin(), orden.end(), srng);
        st.eng.set_plasticity(false);
        st.rd_dorsal.set_lr(lr / (1.0f + 0.05f * ep));
        double r_bits = 0.0;
        size_t r_cnt = 0;

        for (size_t bi : orden) {
            const std::string& full_blk = bloques[bi];
            const size_t pos_sep = full_blk.find(sep_turno);

            st.eng.reset_state();
            wm_acc.clear();
            std::string blk = full_blk;
            if (pos_sep != std::string::npos) {
                const std::string turno1 = full_blk.substr(0, pos_sep);
                const size_t q1_end = turno1.find('\n');
                for (size_t i = 0; i < turno1.size(); ++i) {
                    st.eng.feed((uint8_t)turno1[i]);
                    if (q1_end == std::string::npos || i < q1_end) {
                        st.eng.accumulate_episodic_sdr(wm_acc);
                    }
                }
                st.eng.carry_context(0.38f);
                for (float& x : wm_acc) x *= 0.30f;
                blk = full_blk.substr(pos_sep + sep_turno.size());
            }

            const size_t pos_th = blk.find(tag_th);
            const size_t pos_an = blk.find(tag_an);
            const size_t q_end = (pos_th != std::string::npos) ? pos_th
                               : ((pos_an != std::string::npos) ? pos_an : blk.size());
            size_t ini_sup = 0;
            if (pos_th != std::string::npos) ini_sup = pos_th + tag_th.size();
            else if (pos_an != std::string::npos) ini_sup = pos_an + tag_an.size();
            const size_t ini_an = (pos_an != std::string::npos) ? (pos_an + tag_an.size()) : blk.size();

            q_wm.clear();
            for (size_t i = 0; i + 1 < blk.size(); ++i) {
                st.eng.feed((uint8_t)blk[i]);
                if (i < q_end) {
                    st.eng.accumulate_episodic_sdr(wm_acc);
                    if (i + 1 == q_end) st.eng.finalize_episodic_sdr(wm_acc, q_wm, 192);
                }
                if (i + 1 >= ini_sup) {
                    st.eng.cortical_sparse_features(feats, 2, 2.85f);
                    const size_t dist = (i + 1 >= ini_an) ? (i + 1 - ini_an) : (i + 1 - ini_sup);
                    const float g_wm = 2.90f * std::exp(-(float)dist / 12.0f);
                    if (g_wm > 0.12f) {
                        for (const auto& kv : q_wm) feats.push_back({base_nmda + kv.first, kv.second * g_wm});
                    }
                    const bool arranque = ((i + 1 - ini_sup) < 10) ||
                                          (i + 1 >= ini_an && (i + 1 - ini_an) < 10);
                    const float m = arranque ? 3.5f : 1.0f;
                    r_bits += st.rd_dorsal.step((uint8_t)blk[i + 1], feats, true, m);
                    r_cnt++;
                }
            }
        }
        std::printf("  alineacion epoca %2d/%d: BPC en deliberacion+respuesta = %.3f\n",
                    ep + 1, epocas, r_bits / (double)std::max<size_t>(1, r_cnt));
        std::fflush(stdout);
    }

    guardar_cerebro_completo(ruta_modelo, st);
    std::printf("[GUARDADO] Cerebro deliberativo (ventral intacta + dorsal + CA3) en '%s'.\n", ruta_modelo.c_str());
    return 0;
}

// ============================================================================
// COMANDO 2B: alimentar (aprendizaje continuo por bloques sobre un cerebro vivo)
// ============================================================================
// Disenado para alimentar en cualquier momento nuevos libros, articulos o paquetes
// de razonamiento sobre un cerebro ya existente, consolidando sinapsis bloque a
// bloque con checkpoint automatico en disco y preservando intactos rd_dorsal y CA3.
static int cmd_alimentar(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "Uso: %s alimentar <modelo.bin> <texto1.txt> [texto2.txt ...] "
            "[--bloque 450000] [--epocas 1] [--lr 0.24] [--ancla 0.022] "
            "[--alinear episodios.txt] [--epocas-ali 20] [--eval holdout.txt]\n", argv[0]);
        return 1;
    }
    const std::string ruta_modelo = argv[2];
    if (!existe_archivo(ruta_modelo)) {
        std::fprintf(stderr, "Error: '%s' no existe. 'alimentar' opera siempre en aprendizaje continuo sobre un cerebro existente.\n",
                     ruta_modelo.c_str());
        return 1;
    }

    std::vector<std::string> rutas_texto;
    std::string ruta_ali;
    std::string ruta_eval;
    size_t tam_bloque = 450000;
    int epocas = 1;
    int epocas_ali = 20;
    float lr = 0.24f;
    float ancla = 0.022f;

    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--bloque" && i + 1 < argc) tam_bloque = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--epocas" && i + 1 < argc) epocas = std::atoi(argv[++i]);
        else if (a == "--lr" && i + 1 < argc) lr = std::strtof(argv[++i], nullptr);
        else if (a == "--ancla" && i + 1 < argc) ancla = std::strtof(argv[++i], nullptr);
        else if (a == "--alinear" && i + 1 < argc) ruta_ali = argv[++i];
        else if (a == "--epocas-ali" && i + 1 < argc) epocas_ali = std::atoi(argv[++i]);
        else if (a == "--eval" && i + 1 < argc) ruta_eval = argv[++i];
        else if (!a.empty() && a[0] != '-') rutas_texto.push_back(a);
    }

    EstadoCerebro st;
    cargar_cerebro_completo(ruta_modelo, st);
    const std::string txt_eval = ruta_eval.empty() ? "" : leer_archivo(ruta_eval);

    std::printf("[ALIMENTAR CONTINUO] '%s' | %d regiones | %d neuronas LIF | %zu sinapsis CSR | dorsal=%s | CA3=%zu\n",
                ruta_modelo.c_str(), st.eng.num_layers(), st.eng.n_neurons(), st.eng.n_synapses(),
                st.tiene_dorsal ? "SI" : "NO", st.ca3.num_engrams());
    std::fflush(stdout);

    std::vector<std::pair<i32, float>> feats;
    size_t total_bloques_leidos = 0;
    size_t total_bytes_leidos = 0;

    for (const std::string& rtxt : rutas_texto) {
        const std::string doc = leer_archivo(rtxt);
        if (doc.size() < 64) continue;

        // Partir el documento en bloques alineados a parrafo (\n\n) o caracter UTF-8 limpio
        std::vector<std::pair<size_t, size_t>> tramos;
        size_t p0 = 0;
        while (p0 < doc.size()) {
            if (doc.size() - p0 <= tam_bloque + tam_bloque / 5) {
                tramos.push_back({p0, doc.size()});
                break;
            }
            size_t target = p0 + tam_bloque;
            size_t p_par = doc.find("\n\n", target);
            size_t p1 = (p_par != std::string::npos && p_par < target + 8000) ? (p_par + 2) : target;
            while (p1 < doc.size() && ((uint8_t)doc[p1] & 0xC0) == 0x80) p1++;
            tramos.push_back({p0, p1});
            p0 = p1;
        }

        std::printf("  -> Archivo '%s' (%zu bytes en %zu bloque(s) continuos)\n",
                    rtxt.c_str(), doc.size(), tramos.size());
        std::fflush(stdout);

        for (size_t bi = 0; bi < tramos.size(); ++bi) {
            const size_t s0 = tramos[bi].first;
            const size_t s1 = tramos[bi].second;
            const size_t len = s1 - s0;

            st.rd_ventral.consolidate(ancla);
            const int v0 = st.rd_ventral.active_vocab_size();
            for (size_t i = s0; i < s1; ++i) st.rd_ventral.ensure_byte_slot((uint8_t)doc[i]);
            const int v1 = st.rd_ventral.active_vocab_size();

            for (int ep = 0; ep < epocas; ++ep) {
                auto t0 = std::chrono::steady_clock::now();
                st.eng.reset_state();
                st.eng.set_plasticity(false);
                const float cur_lr = lr / (1.0f + 0.15f * ep);
                st.rd_ventral.set_lr(cur_lr);

                double bits = 0.0;
                size_t cnt = 0;
                for (size_t i = s0; i + 1 < s1; ++i) {
                    st.eng.feed((uint8_t)doc[i]);
                    if (i - s0 >= 32) {
                        st.eng.cortical_sparse_features(feats, 2);
                        bits += st.rd_ventral.step((uint8_t)doc[i + 1], feats, true, 1.0f);
                        cnt++;
                    }
                }
                const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                const double tr_bpc = bits / (double)std::max<size_t>(1, cnt);
                const double cps = (double)len / std::max(1e-6, dt);

                if (!txt_eval.empty()) {
                    const double ho = evaluar_bpc_texto(st.eng, st.rd_ventral, txt_eval, 0);
                    std::printf("     bloque %zu/%zu ep %d/%d (%zu bytes, vocab %d->%d, %.0f chars/s): train=%.3f bpc | eval=%.4f bpc\n",
                                bi + 1, tramos.size(), ep + 1, epocas, len, v0, v1, cps, tr_bpc, ho);
                } else {
                    std::printf("     bloque %zu/%zu ep %d/%d (%zu bytes, vocab %d->%d, %.0f chars/s): train=%.3f bpc\n",
                                bi + 1, tramos.size(), ep + 1, epocas, len, v0, v1, cps, tr_bpc);
                }
                std::fflush(stdout);
            }
            guardar_cerebro_completo(ruta_modelo, st);
            total_bloques_leidos++;
            total_bytes_leidos += len;
        }
    }

    if (!ruta_ali.empty()) {
        const std::string txt_ali = leer_archivo(ruta_ali);
        if (!st.tiene_dorsal) {
            st.rd_dorsal = st.rd_ventral;
            st.rd_dorsal.scale_weights(0.20f);
            st.tiene_dorsal = true;
        }
        st.rd_dorsal.consolidate(0.001f);

        std::vector<std::string> nuevos_bloques;
        size_t p0 = 0;
        while (p0 < txt_ali.size()) {
            while (p0 < txt_ali.size() && (txt_ali[p0] == '\n' || txt_ali[p0] == '\r' || txt_ali[p0] == ' ')) p0++;
            if (p0 >= txt_ali.size()) break;
            size_t p1 = txt_ali.find("\n\n", p0);
            std::string blk = (p1 == std::string::npos) ? txt_ali.substr(p0) : txt_ali.substr(p0, p1 - p0);
            while (!blk.empty() && (blk.back() == '\n' || blk.back() == '\r')) blk.pop_back();
            if (!blk.empty()) nuevos_bloques.push_back(blk + "\n\n");
            if (p1 == std::string::npos) break;
            p0 = p1 + 2;
        }

        // Consolidacion tipo sueno: entrelaza los episodios nuevos con el banco dorsal
        // previo guardado dentro del cerebro (dando doble frecuencia a los nuevos para
        // aprenderlos rapido sin olvidar ninguno de los anteriores).
        std::vector<std::string> bloques = st.dorsal_replay;
        for (const std::string& nb : nuevos_bloques) {
            bloques.push_back(nb);
            bloques.push_back(nb);
        }
        const size_t prev_count = st.dorsal_replay.size();
        agregar_al_banco_dorsal(st.dorsal_replay, nuevos_bloques);
        for (const std::string& b : bloques) {
            for (unsigned char ch : b) st.rd_dorsal.ensure_byte_slot(ch);
        }

        const std::string sep_turno = "\n---\n";
        const std::string tag_th = "\n~ ";
        const std::string tag_an = "\n> ";
        std::mt19937 srng(2027);
        std::vector<size_t> orden(bloques.size());
        for (size_t i = 0; i < orden.size(); ++i) orden[i] = i;

        const i32 base_nmda = st.eng.cortical_feature_dim(2) - st.eng.nmda_concept_dim();
        std::vector<float> wm_acc;
        std::vector<std::pair<i32, float>> q_wm;
        std::printf("  -> Alineacion continua dorsal sobre '%s' (%zu nuevos + %zu previos entrelazados, %d epocas)\n",
                    ruta_ali.c_str(), nuevos_bloques.size(), prev_count, epocas_ali);
        for (int ep = 0; ep < epocas_ali; ++ep) {
            std::shuffle(orden.begin(), orden.end(), srng);
            st.eng.set_plasticity(false);
            st.rd_dorsal.set_lr(0.45f / (1.0f + 0.05f * ep));
            double r_bits = 0.0;
            size_t r_cnt = 0;
            for (size_t bi : orden) {
                const std::string& full_blk = bloques[bi];
                const size_t pos_sep = full_blk.find(sep_turno);
                st.eng.reset_state();
                wm_acc.clear();
                std::string blk = full_blk;
                if (pos_sep != std::string::npos) {
                    const std::string turno1 = full_blk.substr(0, pos_sep);
                    const size_t q1_end = turno1.find('\n');
                    for (size_t i = 0; i < turno1.size(); ++i) {
                        st.eng.feed((uint8_t)turno1[i]);
                        if (q1_end == std::string::npos || i < q1_end) {
                            st.eng.accumulate_episodic_sdr(wm_acc);
                        }
                    }
                    st.eng.carry_context(0.48f);
                    for (float& x : wm_acc) x *= 0.42f;
                    blk = full_blk.substr(pos_sep + sep_turno.size());
                }
                const size_t pos_th = blk.find(tag_th);
                const size_t pos_an = blk.find(tag_an);
                const size_t q_end = (pos_th != std::string::npos) ? pos_th
                                   : ((pos_an != std::string::npos) ? pos_an : blk.size());
                size_t ini_sup = 0;
                if (pos_th != std::string::npos) ini_sup = pos_th + tag_th.size();
                else if (pos_an != std::string::npos) ini_sup = pos_an + tag_an.size();
                const size_t ini_an = (pos_an != std::string::npos) ? (pos_an + tag_an.size()) : blk.size();

                q_wm.clear();
                for (size_t i = 0; i + 1 < blk.size(); ++i) {
                    st.eng.feed((uint8_t)blk[i]);
                    if (i < q_end) {
                        st.eng.accumulate_episodic_sdr(wm_acc);
                        if (i + 1 == q_end) st.eng.finalize_episodic_sdr(wm_acc, q_wm, 192);
                    }
                    if (i + 1 >= ini_sup) {
                        st.eng.cortical_sparse_features(feats, 2, 2.85f);
                        const size_t dist = (i + 1 >= ini_an) ? (i + 1 - ini_an) : (i + 1 - ini_sup);
                        const float g_wm = 2.90f * std::exp(-(float)dist / 12.0f);
                        if (g_wm > 0.12f) {
                            for (const auto& kv : q_wm) feats.push_back({base_nmda + kv.first, kv.second * g_wm});
                        }
                        const bool arranque = ((i + 1 - ini_sup) < 10) ||
                                              (i + 1 >= ini_an && (i + 1 - ini_an) < 10);
                        r_bits += st.rd_dorsal.step((uint8_t)blk[i + 1], feats, true, arranque ? 3.5f : 1.0f);
                        r_cnt++;
                    }
                }
            }
            if ((ep + 1) % 5 == 0 || ep + 1 == epocas_ali) {
                std::printf("     dorsal continua ep %2d/%d: BPC=%.3f\n",
                            ep + 1, epocas_ali, r_bits / (double)std::max<size_t>(1, r_cnt));
                std::fflush(stdout);
            }
        }
        guardar_cerebro_completo(ruta_modelo, st);
    }

    std::printf("[COMPLETADO] Alimentacion continua guardada en '%s' (%zu bloques, %zu bytes nuevos).\n",
                ruta_modelo.c_str(), total_bloques_leidos, total_bytes_leidos);
    return 0;
}

// ============================================================================
// COMANDO 3: recordar (memoria episodica hipocampal CA3 en 1 disparo)
// ============================================================================
static int cmd_recordar(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "Uso: %s recordar <modelo.bin> \"<hecho o recuerdo nuevo>\"\n", argv[0]);
        return 1;
    }
    const std::string ruta_modelo = argv[2];
    const std::string hecho = argv[3];

    auto t0 = std::chrono::steady_clock::now();
    EstadoCerebro st;
    cargar_cerebro_completo(ruta_modelo, st);
    memorizar_episodio_ca3(st, hecho);
    guardar_cerebro_completo(ruta_modelo, st);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[HIPOCAMPO CA3] Engrama #%zu consolidado en %.0f ms: \"%s\"\n",
                st.ca3.num_engrams(), ms, hecho.c_str());
    return 0;
}

// ============================================================================
// COMANDO 4: evaluar (inferencia pura sobre holdout + muestra + diccionario)
// ============================================================================
static int cmd_evaluar(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "Uso: %s evaluar <modelo.bin> <holdout.txt> [dic.txt]\n", argv[0]);
        return 1;
    }
    const std::string ruta_modelo = argv[2];
    const std::string ruta_ho = argv[3];
    const std::string ruta_dic = (argc >= 5) ? argv[4] : "";

    EstadoCerebro st;
    auto t0 = std::chrono::steady_clock::now();
    cargar_cerebro_completo(ruta_modelo, st);
    double load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    const std::string txt = leer_archivo(ruta_ho);
    const double bpc = evaluar_bpc_texto(st.eng, st.rd_ventral, txt, 0);

    std::string ctx = txt.substr(0, std::min<size_t>(120, txt.size()));
    size_t sp = ctx.rfind(' ');
    if (sp != std::string::npos && sp > 30) ctx.resize(sp + 1);
    const std::string muestra = generar_prosa_libre(st.eng, st.rd_ventral, ctx, 300, 0.72, 1234);

    std::printf("Cerebro cargado en %.1f ms | %d regiones | %d neuronas LIF | %zu sinapsis CSR | %zu engramas CA3\n",
                load_ms, st.eng.num_layers(), st.eng.n_neurons(), st.eng.n_synapses(), st.ca3.num_engrams());
    std::printf("HOLDOUT BPC (%zu chars): %.4f bpc\n", txt.size(), bpc);

    if (!ruta_dic.empty()) {
        auto dic = cargar_dic(ruta_dic);
        auto [ok, tot] = contar_palabras_reales(muestra, dic);
        std::printf("Palabras reales (>=3 letras): %d/%d (%.1f%%)\n",
                    ok, tot, tot ? 100.0 * ok / tot : 0.0);
    }
    std::printf("--- CONTINUACION GENERADA ---\n%s%s\n-----------------------------\n",
                ctx.c_str(), muestra.c_str());
    return 0;
}

// ============================================================================
// COMANDO 5: responder (deliberativo por defecto, --pensar, --prev multi-turno)
// ============================================================================
static int cmd_responder(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "Uso: %s responder <modelo.bin> \"<pregunta>\" [--pensar] [--directo] "
            "[--prev \"<turno_previo>\"] [--temp 0.15] [--bytes 280] [--crudo]\n", argv[0]);
        return 1;
    }
    const std::string ruta_modelo = argv[2];
    const std::string entrada_usr = argv[3];
    double temp = 0.15;
    int max_bytes = 280;
    bool crudo = false;
    bool mostrar_pensar = false;
    bool directo = false;
    std::string prev_turno;

    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--temp" && i + 1 < argc) temp = std::strtod(argv[++i], nullptr);
        else if (a == "--bytes" && i + 1 < argc) max_bytes = std::atoi(argv[++i]);
        else if (a == "--crudo") crudo = true;
        else if (a == "--pensar") mostrar_pensar = true;
        else if (a == "--directo") directo = true;
        else if (a == "--prev" && i + 1 < argc) prev_turno = argv[++i];
    }

    EstadoCerebro st;
    cargar_cerebro_completo(ruta_modelo, st);

    if (crudo) {
        const std::string resp = generar_prosa_libre(st.eng, st.rd_ventral, entrada_usr, max_bytes,
                                                     std::max(0.55, temp), 2026);
        std::printf("%s%s\n", entrada_usr.c_str(), resp.c_str());
        return 0;
    }

    bool usar_carry = false;
    if (!prev_turno.empty()) {
        SalidaDeliberativa s1 = responder_cognitivo(st, prev_turno, max_bytes, temp, !directo, false, 2025);
        if (mostrar_pensar) {
            std::printf("[Turno 1] %s\n", prev_turno.c_str());
            if (!s1.pensamiento.empty()) std::printf("  [Pensamiento LIF]: %s\n", s1.pensamiento.c_str());
            std::printf("  [Respuesta 1]: %s\n", s1.respuesta.c_str());
        }
        usar_carry = true;
    }

    SalidaDeliberativa s = responder_cognitivo(st, entrada_usr, max_bytes, temp, !directo, usar_carry, 2026);
    if (mostrar_pensar) {
        if (!s.recuerdo_ca3.empty()) {
            std::printf("[Hipocampo CA3 (sim=%.2f)]: %s\n", s.sim_ca3, s.recuerdo_ca3.c_str());
        }
        if (!s.pensamiento.empty()) {
            std::printf("[Pensamiento LIF]: %s\n", s.pensamiento.c_str());
        }
    }
    std::printf("%s\n", s.respuesta.c_str());
    return 0;
}

// ============================================================================
// COMANDO 6: sesion interactiva multi-turno con memoria NMDA + CA3 en vivo
// ============================================================================
static int cmd_interactivo(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Uso: %s interactivo <modelo.bin> [--temp 0.08] [--pensar]\n", argv[0]);
        return 1;
    }
    const std::string ruta_modelo = argv[2];
    double temp = 0.08;
    bool ver_pensamiento = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--temp" && i + 1 < argc) temp = std::strtod(argv[++i], nullptr);
        else if (a == "--pensar") ver_pensamiento = true;
    }

    EstadoCerebro st;
    cargar_cerebro_completo(ruta_modelo, st);
    std::printf("CORTEX-LM cargado (%d regiones, %d neuronas LIF puras, %zu sinapsis CSR, %zu engramas CA3).\n",
                st.eng.num_layers(), st.eng.n_neurons(), st.eng.n_synapses(), st.ca3.num_engrams());
    std::printf("Comandos: '/pensar' (ver deliberacion), '/recordar <hecho>', '/nuevo' (limpiar hilo), '/continuar <texto>', '/salir'.\n\n");

    std::string linea;
    uint32_t turno = 1;
    bool hay_contexto = false;
    while (true) {
        std::printf("Tu > ");
        std::fflush(stdout);
        if (!std::getline(std::cin, linea)) break;
        if (linea.empty()) continue;
        if (linea == "/salir" || linea == "salir") break;

        if (linea == "/pensar") {
            ver_pensamiento = !ver_pensamiento;
            std::printf("[Modo ver pensamiento interno LIF: %s]\n\n", ver_pensamiento ? "ACTIVO" : "OCULTO");
            continue;
        }
        if (linea == "/nuevo") {
            st.eng.reset_state();
            st.wm_acc.clear();
            hay_contexto = false;
            std::printf("[Hilo conversacional reiniciado]\n\n");
            continue;
        }
        if (linea.rfind("/recordar ", 0) == 0) {
            std::string hecho = linea.substr(10);
            memorizar_episodio_ca3(st, hecho);
            guardar_cerebro_completo(ruta_modelo, st);
            std::printf("[Hipocampo CA3: engrama #%zu consolidado y guardado]\n\n", st.ca3.num_engrams());
            hay_contexto = false;
            continue;
        }
        if (linea.rfind("/continuar ", 0) == 0) {
            std::string ini = linea.substr(11);
            std::string gen = generar_prosa_libre(st.eng, st.rd_ventral, ini, 280, std::max(0.55, temp), 100 + turno++);
            std::printf("CORTEX-LM > %s%s\n\n", ini.c_str(), gen.c_str());
            hay_contexto = false;
            continue;
        }

        SalidaDeliberativa s = responder_cognitivo(st, linea, 280, temp, true, hay_contexto, 100 + turno++);
        if (ver_pensamiento) {
            if (!s.recuerdo_ca3.empty()) {
                std::printf("  [Hipocampo CA3 (sim=%.2f)]: %s\n", s.sim_ca3, s.recuerdo_ca3.c_str());
            }
            if (!s.pensamiento.empty()) {
                std::printf("  [Pensamiento LIF]: %s\n", s.pensamiento.c_str());
            }
        }
        std::printf("CORTEX-LM > %s\n\n", s.respuesta.c_str());
        hay_contexto = true;
    }
    return 0;
}

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    if (argc < 2) {
        std::fprintf(stderr,
            "Uso de cerebro (CORTEX-LM 100%% spiking LIF + 4 regiones + CA3):\n"
            "  %s entrenar    <modelo.bin> <corpus.txt> [opciones]\n"
            "  %s alimentar   <modelo.bin> <nuevo1.txt> [nuevo2.txt ...] [--alinear ep.txt]\n"
            "  %s alinear     <modelo.bin> <alineacion.txt> [opciones]\n"
            "  %s recordar    <modelo.bin> \"<hecho nuevo>\"\n"
            "  %s evaluar     <modelo.bin> <holdout.txt> [dic.txt]\n"
            "  %s responder   <modelo.bin> \"<pregunta>\" [--pensar] [--prev \"<turno1>\"]\n"
            "  %s interactivo <modelo.bin> [--temp 0.18] [--pensar]\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "entrenar") return cmd_entrenar(argc, argv);
    if (cmd == "alimentar") return cmd_alimentar(argc, argv);
    if (cmd == "alinear") return cmd_alinear(argc, argv);
    if (cmd == "recordar") return cmd_recordar(argc, argv);
    if (cmd == "evaluar") return cmd_evaluar(argc, argv);
    if (cmd == "responder") return cmd_responder(argc, argv);
    if (cmd == "interactivo") return cmd_interactivo(argc, argv);
    std::fprintf(stderr, "Comando desconocido: '%s'\n", cmd.c_str());
    return 1;
}
