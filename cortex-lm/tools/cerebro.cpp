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
#include "corteza_viva.hpp"

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

static constexpr uint32_t kMagicBrain      = 0x4E554259u; // "NUBY" (lector denso compatible)
static constexpr uint32_t kMagicMacroBrain = 0x4E554233u; // "NUB3" (lector compacto FP16 macro)
static constexpr uint32_t kMagicDorsal     = 0x43584431u; // "CXD1"
static constexpr uint32_t kMagicHipp       = 0x43584832u; // "CXH2" (CA3 + lector CA1)
static constexpr uint32_t kMagicReplay     = 0x43584231u; // "CXB1" (banco replay sueno dorsal)
static constexpr uint32_t kMagicSemantica  = 0x43584D31u; // "CXM1" (CortezaSemanticaCSR)

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

// Lector unificado que opera de forma transparente en modo denso (compatible bit a bit con
// cerebros NUBY actuales) o en modo compacto FP16 bajo demanda (para >100k..1M+ neuronas LIF).
struct LectorHibrido {
    bool compacto = false;
    CorticalReadout denso;
    CompactCorticalReadout fp16;

    void init(int F, float lr, bool usar_compacto) {
        compacto = usar_compacto;
        if (compacto) fp16.init(F, lr);
        else denso.init(F, lr, 0.0f);
    }
    void set_lr(float lr) {
        if (compacto) fp16.set_lr(lr);
        else denso.set_lr(lr);
    }
    void calibrate_step(const std::vector<std::pair<i32, float>>& feats) {
        if (compacto) fp16.calibrate_step(feats);
        else denso.calibrate_step(feats);
    }
    void freeze_calibration() {
        if (compacto) fp16.freeze_calibration();
        else denso.freeze_calibration();
    }
    double step(int tb, const std::vector<std::pair<i32, float>>& feats, bool learn, float mod = 1.0f) {
        return compacto ? fp16.step(tb, feats, learn, mod) : denso.step(tb, feats, learn, mod);
    }
    void predict(const std::vector<std::pair<i32, float>>& feats, double* probs, double temp = 1.0) const {
        if (compacto) fp16.predict(feats, probs, temp);
        else denso.predict(feats, probs, temp);
    }
    void consolidate(float s = 0.02f) {
        if (compacto) fp16.consolidate(s);
        else denso.consolidate(s);
    }
    void scale_weights(float f) {
        if (compacto) fp16.scale_weights(f);
        else denso.scale_weights(f);
    }
    int ensure_byte_slot(int b) {
        return compacto ? fp16.ensure_byte_slot(b) : denso.ensure_byte_slot(b);
    }
    int active_vocab_size() const {
        return compacto ? fp16.active_vocab_size() : denso.active_vocab_size();
    }
    void save(std::ostream& os) const {
        if (compacto) fp16.save(os);
        else denso.save(os);
    }
    void load(std::istream& is, bool es_compacto) {
        compacto = es_compacto;
        if (compacto) fp16.load(is);
        else denso.load(is);
    }
};

struct EstadoCerebro {
    Engine eng;
    LectorHibrido rd_ventral;
    LectorHibrido rd_dorsal;
    EpisodicCA3 ca3;
    LectorHibrido rd_ca1;
    CortezaSemanticaCSR semantica;
    LectorHibrido rd_sem;
    std::vector<std::string> dorsal_replay;
    std::vector<float> wm_acc;
    bool es_compacto = false;
    bool tiene_dorsal = false;
    bool tiene_ca3 = false;
    bool tiene_semantica = false;
};

static void guardar_cerebro_completo(const std::string& ruta, const EstadoCerebro& st) {
    std::ofstream out(ruta, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "Error: no se puede guardar '%s'\n", ruta.c_str());
        std::exit(1);
    }
    const uint32_t header = st.es_compacto ? kMagicMacroBrain : kMagicBrain;
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    st.eng.save_engine(out);
    st.rd_ventral.save(out);

    if (st.tiene_dorsal) {
        out.write(reinterpret_cast<const char*>(&kMagicDorsal), sizeof(kMagicDorsal));
        st.rd_dorsal.save(out);
    }
    if (st.tiene_ca3 && st.ca3.num_engrams() > 0) {
        out.write(reinterpret_cast<const char*>(&kMagicHipp), sizeof(kMagicHipp));
        st.ca3.save(out);
        st.rd_ca1.save(out);
    }
    if (st.tiene_semantica && st.semantica.num_engrams() > 0) {
        out.write(reinterpret_cast<const char*>(&kMagicSemantica), sizeof(kMagicSemantica));
        st.semantica.save(out);
        st.rd_sem.save(out);
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
    if (magic != kMagicBrain && magic != kMagicMacroBrain) {
        std::fprintf(stderr, "Error: '%s' no es un cerebro CORTEX-LM valido\n", ruta.c_str());
        std::exit(1);
    }
    st.es_compacto = (magic == kMagicMacroBrain);
    st.eng.load_engine(in);
    st.rd_ventral.load(in, st.es_compacto);
    st.tiene_dorsal = false;
    st.tiene_ca3 = false;
    st.tiene_semantica = false;
    st.dorsal_replay.clear();
    st.ca3.init(st.eng.nmda_concept_dim());
    st.semantica.init(st.eng.nmda_concept_dim());

    uint32_t tag = 0;
    while (in.read(reinterpret_cast<char*>(&tag), sizeof(tag))) {
        if (tag == kMagicDorsal) {
            st.rd_dorsal.load(in, st.es_compacto);
            st.tiene_dorsal = true;
        } else if (tag == kMagicHipp) {
            st.ca3.load(in);
            st.rd_ca1.load(in, st.es_compacto);
            st.tiene_ca3 = true;
        } else if (tag == kMagicSemantica) {
            st.semantica.load(in);
            st.rd_sem.load(in, st.es_compacto);
            st.tiene_semantica = (st.semantica.num_engrams() > 0);
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
                                       const LectorHibrido& rd,
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
    std::vector<PremisaRecuperada> premisas_sem;
    std::vector<std::pair<int, float>> columnas_trn;
    EstadoNeuromodulador neuro;
    std::string pensamiento;
    std::string respuesta;
};

static double evaluar_bpc_texto(Engine& eng, LectorHibrido& rd,
                                const std::string& txt, size_t offset_eval = 0);

// Ejecuta el ciclo cognitivo completo 100% spiking LIF ("Corteza Viva"):
//   1) Retencion opcional de contexto NMDA + memoria ejecutiva L2-normalizada entre turnos
//   2) Alimentacion de la pregunta midiendo sorpresa ACh y columnas talamocorticales (TRN)
//   3) Consulta asociativa hipocampal CA3 + CortezaSemanticaCSR con inhibicion lateral
//   4) Compuerta de honestidad tipo Ganglios Basales (Go / No-Go) ante incertidumbre alta
//   5) Deliberacion fisica recurrente con espigas ("\n~ ...") y respuesta ("\n> ...")
static SalidaDeliberativa responder_cognitivo(EstadoCerebro& st,
                                              const std::string& pregunta,
                                              int max_bytes,
                                              double temp,
                                              bool usar_deliberacion,
                                              bool conservar_contexto,
                                              uint32_t seed_gen = 2026,
                                              float umbral_certeza_go = 0.0f) {
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
    const bool tiene_sem_activo = (st.tiene_semantica && st.semantica.num_engrams() > 0);
    std::vector<float> cur_wm;
    std::vector<float> epi_acc;
    std::vector<std::pair<i32, float>> feats;
    double surp_bits = 0.0;
    size_t surp_cnt = 0;
    uint8_t u8_rem = 0;

    for (size_t bi = 0; bi < pregunta.size(); ++bi) {
        const unsigned char b = (unsigned char)pregunta[bi];
        eng.feed(b);
        eng.accumulate_episodic_sdr(cur_wm);
        if (tiene_ca3_activo || tiene_sem_activo) eng.accumulate_episodic_sdr(epi_acc);
        u8_rem = (uint8_t)utf8_rem_after(u8_rem, b);
        if (bi >= 6 && bi + 1 < pregunta.size()) {
            eng.cortical_sparse_features(feats, 2);
            surp_bits += st.rd_ventral.step((uint8_t)pregunta[bi + 1], feats, false, 0.0f);
            surp_cnt++;
        }
    }
    res.columnas_trn = eng.active_macro_columns(-1);

    // Normalizar L2 la memoria de trabajo de la pregunta actual:
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

    std::vector<std::pair<i32, float>> q_sdr;
    if (tiene_ca3_activo || tiene_sem_activo) {
        // Si hay contexto previo, mezclar suavemente para resolver anáforas también en memoria semántica/CA3
        eng.finalize_episodic_sdr(mix_wm, q_sdr, 512);
    }

    const float umbral_mem = (eng.nmda_concept_dim() > 20000) ? 0.15f : 0.20f;
    bool usar_lector_ca1 = false;
    if (tiene_ca3_activo) {
        std::string rec;
        float sim = 0.0f;
        if (st.ca3.recall(q_sdr, rec, umbral_mem, &sim)) {
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

    bool usar_lector_sem = false;
    float max_sim_sem = 0.0f;
    if (!usar_lector_ca1 && tiene_sem_activo) {
        res.premisas_sem = st.semantica.recuperar_premisas(q_sdr, 2, umbral_mem + 0.01f, 0.70f);
        if (!res.premisas_sem.empty()) {
            max_sim_sem = res.premisas_sem[0].similitud;
            std::sort(res.premisas_sem.begin(), res.premisas_sem.end(),
                      [](const PremisaRecuperada& a, const PremisaRecuperada& b) { return a.id < b.id; });
            usar_lector_sem = true;
            if (res.premisas_sem.size() >= 2) {
                const std::string tag_ab = "\n[SINTESIS_A: " + res.premisas_sem[0].texto +
                                           "]\n[SINTESIS_B: " + res.premisas_sem[1].texto + "]";
                for (unsigned char b : tag_ab) {
                    eng.feed(b);
                    u8_rem = (uint8_t)utf8_rem_after(u8_rem, b);
                }
            } else {
                const std::string tag_s = "\n[SEM: " + res.premisas_sem[0].texto + "]";
                for (unsigned char b : tag_s) {
                    eng.feed(b);
                    u8_rem = (uint8_t)utf8_rem_after(u8_rem, b);
                }
            }
        }
    }

    const bool usar_lector_mem = (usar_lector_ca1 || usar_lector_sem);
    const float bpc_in = surp_cnt ? (float)(surp_bits / (double)surp_cnt) : 2.3f;
    const float ach = FisiologiaViva::calcular_acetilcolina(bpc_in, std::max(res.sim_ca3, max_sim_sem));

    const LectorHibrido& rd = usar_lector_ca1 ? st.rd_ca1
                            : (usar_lector_sem ? st.rd_sem
                            : (st.tiene_dorsal ? st.rd_dorsal : st.rd_ventral));
    const float gain_nmda = (usar_lector_mem || st.tiene_dorsal) ? 2.85f : 0.62f;

    std::vector<double> probs(256);
    std::mt19937 rng(seed_gen);

    const bool con_pref_think = (usar_deliberacion && (st.tiene_dorsal || usar_lector_mem));
    const std::string pref_start = con_pref_think ? "\n~ " : "\n> ";
    for (unsigned char b : pref_start) {
        eng.feed(b);
        u8_rem = (uint8_t)utf8_rem_after(u8_rem, b);
    }

    // Evaluar compuerta Go / No-Go exactamente en el punto supervisado de arranque prefrontal ("\n~ ")
    {
        eng.cortical_sparse_features(feats, 2, gain_nmda);
        if (!usar_lector_mem && st.tiene_dorsal) {
            for (const auto& kv : q_wm) feats.push_back({base_nmda + kv.first, kv.second * 2.90f});
        }
        rd.predict(feats, probs.data(), 0.45);
        res.neuro = FisiologiaViva::evaluar_certeza_ganglios(
            probs.data(), std::max(8, rd.active_vocab_size()),
            res.sim_ca3, max_sim_sem, ach, umbral_certeza_go);

        if (umbral_certeza_go > 0.0f && !res.neuro.compuerta_go) {
            char buf_th[256];
            std::snprintf(buf_th, sizeof(buf_th),
                          "ganglios basales No-Go -> certeza=%.2f < %.2f | entropia=%.2f bits | ACh=%.2f -> abstencion honesta",
                          res.neuro.certeza_ganglios, umbral_certeza_go,
                          res.neuro.entropia_arranque, res.neuro.acetilcolina_ach);
            res.pensamiento = buf_th;
            char buf_ans[360];
            std::snprintf(buf_ans, sizeof(buf_ans),
                          "Todavía no tengo recuerdos ni columnas corticales con resonancia suficiente sobre ese punto "
                          "(certeza sináptica %.0f%%, sorpresa ACh %.0f%%). Si me lo explicas en una frase declarativa, "
                          "lo consolidaré en vivo en mi memoria.",
                          100.0 * res.neuro.certeza_ganglios, 100.0 * res.neuro.acetilcolina_ach);
            res.respuesta = buf_ans;
            return res;
        }
    }

    if (con_pref_think) {
        std::string think_buf;
        bool cerro_pensamiento = false;
        const double temp_think = std::min(temp, 0.12);
        for (int g = 0; g < 160; ++g) {
            eng.cortical_sparse_features(feats, 2, gain_nmda);
            if (!usar_lector_mem && st.tiene_dorsal) {
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
    }

    // Generar respuesta audible condicionada por las asambleas L2/L3 encendidas
    std::string out;
    for (int g = 0; g < max_bytes; ++g) {
        eng.cortical_sparse_features(feats, 2, gain_nmda);
        if (!usar_lector_mem && st.tiene_dorsal) {
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

static double evaluar_bpc_texto(Engine& eng, LectorHibrido& rd,
                                const std::string& txt, size_t offset_eval) {
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

static void entrenar_replay_lector(Engine& eng,
                                   LectorHibrido& rd,
                                   const std::vector<std::string>& replay_blks,
                                   int ciclos) {
    if (replay_blks.empty()) return;
    for (const std::string& blk : replay_blks) {
        for (unsigned char b : blk) rd.ensure_byte_slot(b);
    }
    std::vector<std::pair<i32, float>> feats;
    const std::string tag_th = "\n~ ";
    const std::string tag_an = "\n> ";
    std::mt19937 rrng(1337);
    std::vector<size_t> ord(replay_blks.size());
    for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;

    eng.set_plasticity(false);
    for (int rep = 0; rep < ciclos; ++rep) {
        std::shuffle(ord.begin(), ord.end(), rrng);
        rd.set_lr(0.45f / (1.0f + 0.05f * rep));
        for (size_t idx : ord) {
            const std::string& blk = replay_blks[idx];
            const size_t p_th = blk.find(tag_th);
            const size_t p_an = blk.find(tag_an);
            const size_t ini = (p_th != std::string::npos) ? (p_th + tag_th.size()) : 0;
            const size_t ini_an = (p_an != std::string::npos) ? (p_an + tag_an.size()) : blk.size();

            eng.reset_state();
            for (size_t i = 0; i + 1 < blk.size(); ++i) {
                eng.feed((uint8_t)blk[i]);
                if (i + 1 >= ini) {
                    eng.cortical_sparse_features(feats, 2, 2.85f);
                    const bool arranque = ((i + 1 - ini) < 12) ||
                                          (i + 1 >= ini_an && (i + 1 - ini_an) < 18);
                    rd.step((uint8_t)blk[i + 1], feats, true, arranque ? 3.5f : 1.0f);
                }
            }
        }
    }
}

// Consolidacion por Sueno Cortical de Ondas Lentas:
//   - CA3 -> proyeccion hipocampal CA1 (rd_ca1)
//   - CortezaSemanticaCSR -> proyeccion neocortical temporal-prefrontal (rd_sem) con sintesis de 2 premisas
static void consolidar_sueno_cortical(EstadoCerebro& st, int ciclos = 16, size_t max_sem = 24) {
    const size_t n_ca3 = st.ca3.num_engrams();
    const size_t n_sem = st.semantica.num_engrams();

    if (n_ca3 > 0) {
        st.rd_ca1 = st.tiene_dorsal ? st.rd_dorsal : st.rd_ventral;
        st.rd_ca1.scale_weights(0.18f);
        st.tiene_ca3 = true;
        std::vector<std::string> blks_ca3;
        const size_t start_c = (n_ca3 > 20) ? (n_ca3 - 20) : 0;
        for (size_t ei = start_c; ei < n_ca3; ++ei) {
            const std::string& h = st.ca3.engram(ei);
            blks_ca3.push_back(h + "\n[CA3: " + h + "]\n~ memoria CA3 -> " + h +
                               "\n> Según recuerdo: " + h + "\n\n");
            blks_ca3.push_back("¿Qué sabes de eso?\n[CA3: " + h + "]\n~ memoria CA3 -> " + h +
                               "\n> Según recuerdo: " + h + "\n\n");
        }
        entrenar_replay_lector(st.eng, st.rd_ca1, blks_ca3, std::max(14, ciclos));
    }

    if (n_sem > 0) {
        st.rd_sem = st.tiene_dorsal ? st.rd_dorsal : st.rd_ventral;
        st.rd_sem.scale_weights(0.18f);
        st.tiene_semantica = true;
        std::vector<std::string> blks_sem;
        const size_t start_s = (n_sem > max_sem) ? (n_sem - max_sem) : 0;
        for (size_t si = start_s; si < n_sem; ++si) {
            const std::string& p1 = st.semantica.engram(si).texto;
            st.semantica.engram_mut(si).consolidaciones++;
            blks_sem.push_back("Explícame este conocimiento.\n[SEM: " + p1 +
                               "]\n~ [1] premisa cortical -> " + p1 +
                               "\n> Con base en lo estudiado: " + p1 + "\n\n");
            if (si + 1 < n_sem) {
                const std::string& p2 = st.semantica.engram(si + 1).texto;
                const std::string s_blk =
                    "[SINTESIS_A: " + p1 + "]\n[SINTESIS_B: " + p2 +
                    "]\n~ [1] " + p1.substr(0, std::min<size_t>(42, p1.size())) +
                    " | [2] " + p2.substr(0, std::min<size_t>(42, p2.size())) +
                    "\n> Integrando ambos principios: " + p1 + " Además, " + p2 + "\n\n";
                blks_sem.push_back("Relaciona ambos principios.\n" + s_blk);
                blks_sem.push_back("¿Cómo funcionan y cómo se combinan?\n" + s_blk);
            }
        }
        entrenar_replay_lector(st.eng, st.rd_sem, blks_sem, std::max(18, ciclos));
    }
}

// Memorizacion episodica en 1 disparo (CA3 Hebbiano + consolidacion local dedicada en CA1)
static void memorizar_episodio_ca3(EstadoCerebro& st, const std::string& hecho) {
    if (hecho.empty()) return;
    if (!st.tiene_ca3 && st.ca3.num_engrams() == 0) {
        st.ca3.init(st.eng.nmda_concept_dim());
    }
    st.eng.reset_state();
    st.eng.set_plasticity(false);
    std::vector<float> epi_acc;
    for (unsigned char b : hecho) {
        st.eng.feed(b);
        st.eng.accumulate_episodic_sdr(epi_acc);
    }
    std::vector<std::pair<i32, float>> sdr;
    st.eng.finalize_episodic_sdr(epi_acc, sdr, 512);
    st.ca3.memorize(sdr, hecho);
    st.tiene_ca3 = true;

    st.rd_ca1 = st.tiene_dorsal ? st.rd_dorsal : st.rd_ventral;
    st.rd_ca1.scale_weights(0.18f);
    std::vector<std::string> blks_ca3;
    const size_t n_ca3 = st.ca3.num_engrams();
    const size_t start_c = (n_ca3 > 20) ? (n_ca3 - 20) : 0;
    for (size_t ei = start_c; ei < n_ca3; ++ei) {
        const std::string& h = st.ca3.engram(ei);
        blks_ca3.push_back(h + "\n[CA3: " + h + "]\n~ memoria CA3 -> " + h +
                           "\n> Según recuerdo: " + h + "\n\n");
        blks_ca3.push_back("¿Qué sabes de eso?\n[CA3: " + h + "]\n~ memoria CA3 -> " + h +
                           "\n> Según recuerdo: " + h + "\n\n");
    }
    entrenar_replay_lector(st.eng, st.rd_ca1, blks_ca3, 16);
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
    int macro_cols = 0;
    int trn_top_k = 4;
    bool forzar_compacto = false;

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
        else if (a == "--columnas" && i + 1 < argc) macro_cols = std::atoi(argv[++i]);
        else if (a == "--top-columnas" && i + 1 < argc) trn_top_k = std::atoi(argv[++i]);
        else if (a == "--compacto" || a == "--macro") forzar_compacto = true;
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
        p.ticks_per_char = (p.total_neurons() >= 100000) ? 6 : 8;
        p.n_macro_cols = macro_cols;
        p.trn_top_k = trn_top_k;
        if (p.total_neurons() >= 40000) p.assoc_fan_in_slot = 4;
        if (!stdp) {
            p.stdp_ltp = 0.0f;
            p.stdp_ltd = 0.0f;
            p.homeo_thr_up = 0.0f;
            p.homeo_thr_down = 0.0f;
        }
        st.eng = Engine(p);
        st.eng.build_random(seed);
        st.es_compacto = forzar_compacto || (st.eng.n_neurons() > 60000);
        st.rd_ventral.init(st.eng.cortical_feature_dim(2), lr, st.es_compacto);
        st.ca3.init(st.eng.nmda_concept_dim());
        st.semantica.init(st.eng.nmda_concept_dim());
        for (size_t i = 0; i < NTR; ++i) st.rd_ventral.ensure_byte_slot((uint8_t)data[i]);

        std::printf("[NUEVO] Cerebro LIF puro %s (%d regiones%s%s, %d hilos CPU, lector=%s) | %d neuronas | %zu sinapsis CSR | train=%zu chars\n",
                    capas_str.c_str(), st.eng.num_layers(),
                    st.eng.num_layers() >= 4 ? " + penacho apical Larkum" : "",
                    macro_cols > 1 ? (" + talamo TRN " + std::to_string(trn_top_k) + "/" + std::to_string(macro_cols)).c_str() : "",
                    Engine::max_cpu_threads(),
                    st.es_compacto ? "FP16-compacto" : "FP32",
                    st.eng.n_neurons(), st.eng.n_synapses(), NTR);
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
            "[--alinear episodios.txt] [--epocas-ali 20] [--eval holdout.txt] "
            "[--plasticidad-local|--stdp-local|--homeostasis-local]\n", argv[0]);
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
    bool indexar_semantica = false;
    bool plasticidad_local = false;
    bool stdp_local = false;
    bool homeostasis_local = false;
    size_t max_pasajes_sem = 24;

    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--bloque" && i + 1 < argc) tam_bloque = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--epocas" && i + 1 < argc) epocas = std::atoi(argv[++i]);
        else if (a == "--lr" && i + 1 < argc) lr = std::strtof(argv[++i], nullptr);
        else if (a == "--ancla" && i + 1 < argc) ancla = std::strtof(argv[++i], nullptr);
        else if (a == "--alinear" && i + 1 < argc) ruta_ali = argv[++i];
        else if (a == "--epocas-ali" && i + 1 < argc) epocas_ali = std::atoi(argv[++i]);
        else if (a == "--eval" && i + 1 < argc) ruta_eval = argv[++i];
        else if (a == "--semantica") indexar_semantica = true;
        else if (a == "--plasticidad-local") plasticidad_local = true;
        else if (a == "--stdp-local") stdp_local = true;
        else if (a == "--homeostasis-local") homeostasis_local = true;
        else if (a == "--max-pasajes" && i + 1 < argc) max_pasajes_sem = std::strtoull(argv[++i], nullptr, 10);
        else if (!a.empty() && a[0] != '-') rutas_texto.push_back(a);
    }

    EstadoCerebro st;
    cargar_cerebro_completo(ruta_modelo, st);
    const std::string txt_eval = ruta_eval.empty() ? "" : leer_archivo(ruta_eval);

    const bool activar_stdp = plasticidad_local || stdp_local;
    const bool activar_homeo = plasticidad_local || homeostasis_local;
    std::printf("[ALIMENTAR CONTINUO] '%s' | %d regiones | %d neuronas LIF | %zu sinapsis CSR | dorsal=%s | CA3=%zu | STDP-local=%s | homeostasis-local=%s\n",
                ruta_modelo.c_str(), st.eng.num_layers(), st.eng.n_neurons(), st.eng.n_synapses(),
                st.tiene_dorsal ? "SI" : "NO", st.ca3.num_engrams(),
                activar_stdp ? "SI" : "NO", activar_homeo ? "SI" : "NO");
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
                st.eng.set_stdp_enabled(activar_stdp);
                st.eng.set_homeostasis_enabled(activar_homeo);
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
            if (indexar_semantica) {
                const std::string sub = doc.substr(s0, len);
                const size_t n_as = st.semantica.asimilar_documento_vivo(st.eng, sub, nullptr, max_pasajes_sem);
                if (n_as > 0) {
                    st.tiene_semantica = true;
                    std::printf("     [Leer=Saber CSR] +%zu engramas semanticos indexados (total=%zu)\n",
                                n_as, st.semantica.num_engrams());
                }
            }
            guardar_cerebro_completo(ruta_modelo, st);
            total_bloques_leidos++;
            total_bytes_leidos += len;
        }
        if (indexar_semantica && st.semantica.num_engrams() > 0) {
            consolidar_sueno_cortical(st, 8, 20);
            guardar_cerebro_completo(ruta_modelo, st);
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
    int max_bytes = 420;
    bool crudo = false;
    bool mostrar_pensar = false;
    bool directo = false;
    float umbral_certeza = 0.0f;
    std::string prev_turno;

    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--temp" && i + 1 < argc) temp = std::strtod(argv[++i], nullptr);
        else if (a == "--bytes" && i + 1 < argc) max_bytes = std::atoi(argv[++i]);
        else if (a == "--crudo") crudo = true;
        else if (a == "--pensar") mostrar_pensar = true;
        else if (a == "--directo") directo = true;
        else if (a == "--honesto") umbral_certeza = 0.20f;
        else if (a == "--umbral-certeza" && i + 1 < argc) umbral_certeza = std::strtof(argv[++i], nullptr);
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
        SalidaDeliberativa s1 = responder_cognitivo(st, prev_turno, max_bytes, temp, !directo, false, 2025, umbral_certeza);
        if (mostrar_pensar) {
            std::printf("[Turno 1] %s\n", prev_turno.c_str());
            if (!s1.pensamiento.empty()) std::printf("  [Pensamiento LIF]: %s\n", s1.pensamiento.c_str());
            std::printf("  [Respuesta 1]: %s\n", s1.respuesta.c_str());
        }
        usar_carry = true;
    }

    SalidaDeliberativa s = responder_cognitivo(st, entrada_usr, max_bytes, temp, !directo, usar_carry, 2026, umbral_certeza);
    if (mostrar_pensar) {
        if (!s.columnas_trn.empty()) {
            std::printf("[Talamo TRN]: ");
            for (size_t k = 0; k < s.columnas_trn.size(); ++k)
                std::printf("%sCol#%d(%.1f)", k ? ", " : "", s.columnas_trn[k].first, s.columnas_trn[k].second);
            std::printf(" | ACh=%.2f | Certeza=%.2f (%s)\n",
                        s.neuro.acetilcolina_ach, s.neuro.certeza_ganglios,
                        s.neuro.compuerta_go ? "Go" : "No-Go");
        }
        if (!s.recuerdo_ca3.empty()) {
            std::printf("[Hipocampo CA3 (sim=%.2f)]: %s\n", s.sim_ca3, s.recuerdo_ca3.c_str());
        }
        for (size_t pi = 0; pi < s.premisas_sem.size(); ++pi) {
            std::printf("[Corteza Semantica #%zu (sim=%.2f)]: %s\n",
                        pi + 1, s.premisas_sem[pi].similitud, s.premisas_sem[pi].texto.c_str());
        }
        if (!s.pensamiento.empty()) {
            std::printf("[Pensamiento LIF]: %s\n", s.pensamiento.c_str());
        }
    }
    std::printf("%s\n", s.respuesta.c_str());
    return 0;
}

// ============================================================================
// COMANDO 6: asimilar ("Leer = Saber" indexacion semantica CSR + sintesis)
// ============================================================================
static int cmd_asimilar(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "Uso: %s asimilar <modelo.bin> <doc1.txt> [doc2.txt ...] [--max-pasajes 60] [--ciclos 12]\n",
            argv[0]);
        return 1;
    }
    const std::string ruta_modelo = argv[2];
    size_t max_pasajes = 60;
    int ciclos = 12;
    std::vector<std::string> rutas;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--max-pasajes" && i + 1 < argc) max_pasajes = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--ciclos" && i + 1 < argc) ciclos = std::atoi(argv[++i]);
        else if (!a.empty() && a[0] != '-') rutas.push_back(a);
    }

    auto t0 = std::chrono::steady_clock::now();
    EstadoCerebro st;
    cargar_cerebro_completo(ruta_modelo, st);
    size_t total_nuevos = 0;
    for (const std::string& r : rutas) {
        const std::string txt = leer_archivo(r);
        const size_t n = st.semantica.asimilar_documento_vivo(st.eng, txt, nullptr, max_pasajes);
        total_nuevos += n;
        std::printf("  [Corteza Semantica CSR] '%s' -> +%zu engramas (total=%zu)\n",
                    r.c_str(), n, st.semantica.num_engrams());
    }
    if (st.semantica.num_engrams() > 0) {
        st.tiene_semantica = true;
        consolidar_sueno_cortical(st, ciclos, 24);
        guardar_cerebro_completo(ruta_modelo, st);
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[ASIMILADO] +%zu engramas semanticos y sintesis consolidada en %.1fs (%zu engramas totales).\n",
                total_nuevos, s, st.semantica.num_engrams());
    return 0;
}

// ============================================================================
// COMANDO 7: sonar (Sueno Cortical de Ondas Lentas: replay CA3 + Semantica)
// ============================================================================
static int cmd_sonar(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Uso: %s sonar <modelo.bin> [--ciclos 14]\n", argv[0]);
        return 1;
    }
    const std::string ruta_modelo = argv[2];
    int ciclos = 14;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--ciclos" && i + 1 < argc) ciclos = std::atoi(argv[++i]);
    }
    auto t0 = std::chrono::steady_clock::now();
    EstadoCerebro st;
    cargar_cerebro_completo(ruta_modelo, st);
    consolidar_sueno_cortical(st, ciclos, 32);
    guardar_cerebro_completo(ruta_modelo, st);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[SUENO CORTICAL] %d ciclos completados en %.2fs (CA3=%zu, Semantica=%zu engramas).\n",
                ciclos, s, st.ca3.num_engrams(), st.semantica.num_engrams());
    return 0;
}

static std::string escapar_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) continue;
        else out.push_back((char)c);
    }
    return out;
}

// ============================================================================
// COMANDO 8: tubo (demonio JSON-Lines persistente 24/7 para Observatorio Web)
// ============================================================================
static int cmd_tubo(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Uso: %s tubo <modelo.bin> [--temp 0.08] [--honesto]\n", argv[0]);
        return 1;
    }
    const std::string ruta_modelo = argv[2];
    double temp = 0.08;
    float umbral_certeza = 0.0f;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--temp" && i + 1 < argc) temp = std::strtod(argv[++i], nullptr);
        else if (a == "--honesto") umbral_certeza = 0.28f;
    }

    EstadoCerebro st;
    cargar_cerebro_completo(ruta_modelo, st);
    std::printf("{\"evento\":\"listo\",\"regiones\":%d,\"neuronas\":%d,\"sinapsis\":%zu,"
                "\"ca3\":%zu,\"semantica\":%zu,\"compacto\":%s,\"hilos\":%d}\n",
                st.eng.num_layers(), st.eng.n_neurons(), st.eng.n_synapses(),
                st.ca3.num_engrams(), st.semantica.num_engrams(),
                st.es_compacto ? "true" : "false", Engine::max_cpu_threads());
    std::fflush(stdout);

    std::string linea;
    uint32_t turno = 1;
    bool hay_contexto = false;
    bool auto_vivo = true;

    while (std::getline(std::cin, linea)) {
        if (linea.empty()) continue;
        if (linea == "/salir") break;
        if (linea == "/nuevo") {
            st.eng.reset_state();
            st.wm_acc.clear();
            hay_contexto = false;
            std::printf("{\"evento\":\"reiniciado\",\"respuesta\":\"Hilo conversacional reiniciado.\"}\n");
            std::fflush(stdout);
            continue;
        }
        if (linea == "/sonar") {
            consolidar_sueno_cortical(st, 12, 24);
            guardar_cerebro_completo(ruta_modelo, st);
            std::printf("{\"evento\":\"sueno\",\"ca3\":%zu,\"semantica\":%zu,"
                        "\"respuesta\":\"Sueño cortical de ondas lentas completado y guardado.\"}\n",
                        st.ca3.num_engrams(), st.semantica.num_engrams());
            std::fflush(stdout);
            continue;
        }
        if (linea.rfind("/recordar ", 0) == 0) {
            std::string h = linea.substr(10);
            memorizar_episodio_ca3(st, h);
            guardar_cerebro_completo(ruta_modelo, st);
            hay_contexto = false;
            std::printf("{\"evento\":\"recordado\",\"ca3\":%zu,\"respuesta\":\"Consolidado en Hipocampo CA3: %s\"}\n",
                        st.ca3.num_engrams(), escapar_json(h).c_str());
            std::fflush(stdout);
            continue;
        }
        if (linea.rfind("/asimilar ", 0) == 0) {
            std::string doc = linea.substr(10);
            size_t n = st.semantica.asimilar_documento_vivo(st.eng, doc, nullptr, 20);
            if (n == 0 && doc.size() >= 18) {
                memorizar_episodio_ca3(st, doc);
                guardar_cerebro_completo(ruta_modelo, st);
                std::printf("{\"evento\":\"asimilado\",\"nuevos\":1,\"semantica\":%zu,\"ca3\":%zu,"
                            "\"respuesta\":\"Dato consolidado en memoria episódica viva.\"}\n",
                            st.semantica.num_engrams(), st.ca3.num_engrams());
            } else {
                st.tiene_semantica = (st.semantica.num_engrams() > 0);
                consolidar_sueno_cortical(st, 10, 20);
                guardar_cerebro_completo(ruta_modelo, st);
                std::printf("{\"evento\":\"asimilado\",\"nuevos\":%zu,\"semantica\":%zu,\"ca3\":%zu,"
                            "\"respuesta\":\"Asimilados %zu pasajes en Corteza Semántica CSR y consolidados por sueño cortical.\"}\n",
                            n, st.semantica.num_engrams(), st.ca3.num_engrams(), n);
            }
            hay_contexto = false;
            std::fflush(stdout);
            continue;
        }

        SalidaDeliberativa s = responder_cognitivo(st, linea, 420, temp, true, hay_contexto,
                                                   500 + turno++, umbral_certeza);
        bool aprendio_vivo = false;
        if (auto_vivo && FisiologiaViva::debe_aprender_autonomamente(linea, s.neuro.acetilcolina_ach, s.sim_ca3)) {
            memorizar_episodio_ca3(st, linea);
            guardar_cerebro_completo(ruta_modelo, st);
            aprendio_vivo = true;
            s.pensamiento = "sorpresa ACh=" + std::to_string(s.neuro.acetilcolina_ach).substr(0, 4) +
                            " -> plasticidad autonoma CA3 -> " + linea;
            s.respuesta = "He registrado esa información nueva directamente en mi memoria sináptica viva: \"" +
                          linea + "\". Puedes preguntarme sobre ello cuando quieras.";
            hay_contexto = false;
        } else {
            hay_contexto = true;
        }

        std::printf("{\"evento\":\"respuesta\",\"respuesta\":\"%s\",\"pensamiento\":\"%s\","
                    "\"recuerdo_ca3\":\"%s\",\"sim_ca3\":%.3f,\"ach\":%.3f,\"certeza\":%.3f,"
                    "\"go\":%s,\"aprendio_vivo\":%s,\"ca3\":%zu,\"semantica\":%zu,\"premisas\":[",
                    escapar_json(s.respuesta).c_str(),
                    escapar_json(s.pensamiento).c_str(),
                    escapar_json(s.recuerdo_ca3).c_str(),
                    s.sim_ca3, s.neuro.acetilcolina_ach, s.neuro.certeza_ganglios,
                    s.neuro.compuerta_go ? "true" : "false",
                    aprendio_vivo ? "true" : "false",
                    st.ca3.num_engrams(), st.semantica.num_engrams());
        for (size_t pi = 0; pi < s.premisas_sem.size(); ++pi) {
            std::printf("%s{\"texto\":\"%s\",\"sim\":%.3f}",
                        pi ? "," : "",
                        escapar_json(s.premisas_sem[pi].texto).c_str(),
                        s.premisas_sem[pi].similitud);
        }
        std::printf("],\"columnas\":[");
        for (size_t ki = 0; ki < s.columnas_trn.size(); ++ki) {
            std::printf("%s{\"id\":%d,\"energia\":%.2f}",
                        ki ? "," : "", s.columnas_trn[ki].first, s.columnas_trn[ki].second);
        }
        std::printf("]}\n");
        std::fflush(stdout);
    }
    return 0;
}

// ============================================================================
// COMANDO 9: sesion interactiva multi-turno con Corteza Viva (ACh + CA3 + CSR)
// ============================================================================
static int cmd_interactivo(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Uso: %s interactivo <modelo.bin> [--temp 0.08] [--pensar] [--vivo] [--honesto]\n", argv[0]);
        return 1;
    }
    const std::string ruta_modelo = argv[2];
    double temp = 0.08;
    bool ver_pensamiento = false;
    bool plasticidad_viva = false;
    float umbral_certeza = 0.0f;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--temp" && i + 1 < argc) temp = std::strtod(argv[++i], nullptr);
        else if (a == "--pensar") ver_pensamiento = true;
        else if (a == "--vivo") plasticidad_viva = true;
        else if (a == "--honesto") umbral_certeza = 0.28f;
    }

    EstadoCerebro st;
    cargar_cerebro_completo(ruta_modelo, st);
    std::printf("CORTEX-LM cargado (%d regiones, %d neuronas LIF puras, %zu sinapsis CSR, %zu CA3, %zu semanticos).\n",
                st.eng.num_layers(), st.eng.n_neurons(), st.eng.n_synapses(),
                st.ca3.num_engrams(), st.semantica.num_engrams());
    std::printf("Comandos: '/pensar', '/vivo' (plasticidad autonoma ACh), '/honesto', '/sonar', '/recordar <hecho>', '/nuevo', '/continuar <texto>', '/salir'.\n\n");

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
        if (linea == "/vivo") {
            plasticidad_viva = !plasticidad_viva;
            std::printf("[Plasticidad autonoma por sorpresa ACh: %s]\n\n", plasticidad_viva ? "ACTIVA" : "MANUAL");
            continue;
        }
        if (linea == "/honesto") {
            umbral_certeza = (umbral_certeza > 0.0f) ? 0.0f : 0.28f;
            std::printf("[Compuerta Ganglios Basales Go/No-Go: %s]\n\n", umbral_certeza > 0.0f ? "ACTIVA" : "DESACTIVADA");
            continue;
        }
        if (linea == "/sonar") {
            consolidar_sueno_cortical(st, 12, 24);
            guardar_cerebro_completo(ruta_modelo, st);
            std::printf("[Sueno Cortical completado: CA3=%zu, Semantica=%zu]\n\n",
                        st.ca3.num_engrams(), st.semantica.num_engrams());
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

        SalidaDeliberativa s = responder_cognitivo(st, linea, 420, temp, true, hay_contexto,
                                                   100 + turno++, umbral_certeza);
        if (plasticidad_viva &&
            FisiologiaViva::debe_aprender_autonomamente(linea, s.neuro.acetilcolina_ach, s.sim_ca3)) {
            memorizar_episodio_ca3(st, linea);
            guardar_cerebro_completo(ruta_modelo, st);
            std::printf("  [Plasticidad Viva ACh=%.2f -> consolidado autonomamente en CA3 (#%zu)]\n",
                        s.neuro.acetilcolina_ach, st.ca3.num_engrams());
            std::printf("CORTEX-LM > He consolidado ese nuevo recuerdo en mi memoria sináptica viva. Pregúntame cuando quieras.\n\n");
            hay_contexto = false;
            continue;
        }

        if (ver_pensamiento) {
            if (!s.columnas_trn.empty()) {
                std::printf("  [Talamo TRN]: ");
                for (size_t k = 0; k < s.columnas_trn.size(); ++k)
                    std::printf("%sCol#%d(%.1f)", k ? ", " : "", s.columnas_trn[k].first, s.columnas_trn[k].second);
                std::printf(" | ACh=%.2f | Certeza=%.2f\n", s.neuro.acetilcolina_ach, s.neuro.certeza_ganglios);
            }
            if (!s.recuerdo_ca3.empty()) {
                std::printf("  [Hipocampo CA3 (sim=%.2f)]: %s\n", s.sim_ca3, s.recuerdo_ca3.c_str());
            }
            for (size_t pi = 0; pi < s.premisas_sem.size(); ++pi) {
                std::printf("  [Corteza Semantica #%zu (sim=%.2f)]: %s\n",
                            pi + 1, s.premisas_sem[pi].similitud, s.premisas_sem[pi].texto.c_str());
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
            "Uso de cerebro (CORTEX-LM 100%% spiking LIF — Corteza Viva Macro-Escala):\n"
            "  %s entrenar    <modelo.bin> <corpus.txt> [--capas ...] [--columnas 32] [--macro]\n"
            "  %s alimentar   <modelo.bin> <nuevo1.txt> [...] [--semantica] [--alinear ep.txt]\n"
            "  %s asimilar    <modelo.bin> <doc1.txt> [...] [--max-pasajes 60]\n"
            "  %s sonar       <modelo.bin> [--ciclos 14]\n"
            "  %s alinear     <modelo.bin> <alineacion.txt> [opciones]\n"
            "  %s recordar    <modelo.bin> \"<hecho nuevo>\"\n"
            "  %s evaluar     <modelo.bin> <holdout.txt> [dic.txt]\n"
            "  %s responder   <modelo.bin> \"<pregunta>\" [--pensar] [--honesto] [--prev \"<t1>\"]\n"
            "  %s interactivo <modelo.bin> [--temp 0.08] [--pensar] [--vivo] [--honesto]\n"
            "  %s tubo        <modelo.bin> [--temp 0.08] [--honesto]\n",
            argv[0], argv[0], argv[0], argv[0], argv[0],
            argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "entrenar") return cmd_entrenar(argc, argv);
    if (cmd == "alimentar") return cmd_alimentar(argc, argv);
    if (cmd == "asimilar") return cmd_asimilar(argc, argv);
    if (cmd == "sonar") return cmd_sonar(argc, argv);
    if (cmd == "alinear") return cmd_alinear(argc, argv);
    if (cmd == "recordar") return cmd_recordar(argc, argv);
    if (cmd == "evaluar") return cmd_evaluar(argc, argv);
    if (cmd == "responder") return cmd_responder(argc, argv);
    if (cmd == "interactivo") return cmd_interactivo(argc, argv);
    if (cmd == "tubo") return cmd_tubo(argc, argv);
    std::fprintf(stderr, "Comando desconocido: '%s'\n", cmd.c_str());
    return 1;
}
