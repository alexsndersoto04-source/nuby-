// corteza_viva.cpp — Implementacion 100% spiking SDR + grafos sinapticos invertidos
// de CortezaSemanticaCSR y FisiologiaViva (ACh, Ganglios Basales Go/No-Go).

#include "corteza_viva.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <unordered_map>

namespace cortex {

namespace {

template <typename T>
void wr_pod(std::ostream& os, const T& v) {
    os.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <typename T>
void rd_pod(std::istream& is, T& v) {
    is.read(reinterpret_cast<char*>(&v), sizeof(T));
}

static std::string limpiar_espacios(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prev_sp = true;
    for (unsigned char c : s) {
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!prev_sp) {
                out.push_back(' ');
                prev_sp = true;
            }
        } else {
            out.push_back((char)c);
            prev_sp = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

static float coseno_sdr_ordenado(const std::vector<std::pair<i32, float>>& a,
                                 const std::vector<std::pair<i32, float>>& b) {
    size_t ia = 0, ib = 0;
    float dot = 0.0f;
    while (ia < a.size() && ib < b.size()) {
        if (a[ia].first == b[ib].first) {
            dot += a[ia].second * b[ib].second;
            ++ia;
            ++ib;
        } else if (a[ia].first < b[ib].first) {
            ++ia;
        } else {
            ++ib;
        }
    }
    return dot;
}

} // namespace

void CortezaSemanticaCSR::init(int concept_dim) {
    dim_ = std::max(0, concept_dim);
    engrams_.clear();
    syn_inv_.assign((size_t)dim_, {});
    score_buf_.clear();
    touched_.clear();
}

int CortezaSemanticaCSR::buscar_mejor_interno(const std::vector<std::pair<i32, float>>& q_sdr,
                                              const std::vector<int>& excluidos,
                                              float& best_score) const {
    best_score = 0.0f;
    if (engrams_.empty() || q_sdr.empty()) return -1;
    if (score_buf_.size() < engrams_.size()) score_buf_.assign(engrams_.size(), 0.0f);
    touched_.clear();

    for (const auto& kv : q_sdr) {
        const i32 c = kv.first;
        if (c < 0 || c >= dim_) continue;
        const float qw = kv.second;
        for (const auto& syn : syn_inv_[(size_t)c]) {
            const i32 eid = syn.first;
            if (score_buf_[(size_t)eid] == 0.0f) touched_.push_back(eid);
            score_buf_[(size_t)eid] += qw * syn.second;
        }
    }

    int best_id = -1;
    for (i32 eid : touched_) {
        const float sc = score_buf_[(size_t)eid];
        score_buf_[(size_t)eid] = 0.0f;
        bool skip = false;
        for (int ex : excluidos) {
            if (ex == (int)eid) { skip = true; break; }
        }
        if (!skip && sc > best_score) {
            best_score = sc;
            best_id = (int)eid;
        }
    }
    return best_id;
}

int CortezaSemanticaCSR::asimilar_pasaje_sdr(const std::vector<std::pair<i32, float>>& sdr_in,
                                             const std::string& texto_limpio,
                                             int macro_col,
                                             float sorpresa_ach,
                                             float max_dup_sim) {
    if (dim_ <= 0 || sdr_in.empty() || texto_limpio.size() < 20) return -1;

    std::vector<std::pair<i32, float>> sdr = sdr_in;
    float sq = 0.0f;
    for (const auto& kv : sdr) sq += kv.second * kv.second;
    if (sq <= 1e-10f) return -1;
    const float inv = 1.0f / std::sqrt(sq);
    for (auto& kv : sdr) kv.second *= inv;
    std::sort(sdr.begin(), sdr.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    float dup_sim = 0.0f;
    const int prev_id = buscar_mejor_interno(sdr, {}, dup_sim);
    if (prev_id >= 0 && dup_sim >= max_dup_sim) {
        engrams_[(size_t)prev_id].consolidaciones++;
        return -1;
    }

    const int new_id = (int)engrams_.size();
    EngramaSemantico eg;
    eg.texto = texto_limpio;
    eg.macro_columna = macro_col;
    eg.sorpresa_ach = sorpresa_ach;
    eg.consolidaciones = 1;
    eg.sdr = sdr;
    engrams_.push_back(std::move(eg));
    score_buf_.push_back(0.0f);

    for (const auto& kv : engrams_.back().sdr) {
        if (kv.first >= 0 && kv.first < dim_) {
            syn_inv_[(size_t)kv.first].push_back({(i32)new_id, kv.second});
        }
    }
    return new_id;
}

std::vector<std::string> FisiologiaViva::extraer_enunciados_informativos(const std::string& doc) {
    std::vector<std::string> out;
    size_t n = doc.size();
    size_t ini = 0;
    while (ini < n) {
        while (ini < n && (doc[ini] == ' ' || doc[ini] == '\n' || doc[ini] == '\r' || doc[ini] == '\t')) ini++;
        if (ini >= n) break;

        size_t fin = ini;
        while (fin < n) {
            const char c = doc[fin];
            if (c == '\n') {
                if (fin + 1 < n && doc[fin + 1] == '\n') break;
                if (fin - ini >= 55) break;
            }
            if ((c == '.' || c == '!' || c == ';') && (fin - ini >= 42)) {
                if (fin + 1 >= n || doc[fin + 1] == ' ' || doc[fin + 1] == '\n' || doc[fin + 1] == '\r') {
                    fin++;
                    break;
                }
            }
            if (fin - ini >= 280 && c == ' ') break;
            fin++;
        }

        std::string limpio = limpiar_espacios(doc.substr(ini, fin - ini));
        // Filtrar lineas de dialogo teatral ("--...") o cabeceras muy cortas
        if (limpio.size() >= 36 && limpio.size() <= 300 &&
            limpio.rfind("--", 0) != 0 && limpio.find('?') == std::string::npos) {
            out.push_back(std::move(limpio));
        }
        ini = (fin > ini) ? fin : (ini + 1);
    }
    return out;
}

size_t CortezaSemanticaCSR::asimilar_documento_vivo(
    Engine& eng,
    const std::string& documento,
    const std::function<float(const std::string&)>& medidor_bpc,
    size_t max_nuevos) {
    if (dim_ <= 0) init(eng.nmda_concept_dim());
    const auto pasajes = FisiologiaViva::extraer_enunciados_informativos(documento);

    size_t agregados = 0;
    std::vector<float> acc;
    std::vector<std::pair<i32, float>> sdr;
    const bool prev_plast = eng.plasticity_enabled();
    eng.set_plasticity(false);

    for (const std::string& p : pasajes) {
        if (max_nuevos > 0 && agregados >= max_nuevos) break;
        eng.reset_state();
        acc.assign((size_t)dim_, 0.0f);
        for (unsigned char b : p) {
            eng.feed(b);
            eng.accumulate_episodic_sdr(acc);
        }
        eng.finalize_episodic_sdr(acc, sdr, 512);
        if (sdr.empty()) continue;

        int dom_col = -1;
        auto cols = eng.active_macro_columns(-1);
        if (!cols.empty()) dom_col = cols[0].first;

        float bpc_est = medidor_bpc ? medidor_bpc(p) : 2.2f;
        float prev_sim = 0.0f;
        buscar_mejor_interno(sdr, {}, prev_sim);
        const float ach = FisiologiaViva::calcular_acetilcolina(bpc_est, prev_sim);

        if (asimilar_pasaje_sdr(sdr, p, dom_col, ach, 0.89f) >= 0) {
            agregados++;
        }
    }

    eng.set_plasticity(prev_plast);
    return agregados;
}

std::vector<PremisaRecuperada> CortezaSemanticaCSR::recuperar_premisas(
    const std::vector<std::pair<i32, float>>& query_sdr,
    int top_k,
    float min_sim,
    float inhibicion_lateral) const {
    std::vector<PremisaRecuperada> res;
    if (engrams_.empty() || query_sdr.empty() || top_k <= 0) return res;

    std::vector<std::pair<i32, float>> q_work = query_sdr;
    std::sort(q_work.begin(), q_work.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<i32, float>> q_orig = q_work;
    std::vector<int> excluidos;

    for (int paso = 0; paso < top_k; ++paso) {
        float sc_work = 0.0f;
        const int best = buscar_mejor_interno(q_work, excluidos, sc_work);
        if (best < 0) break;

        const auto& eg = engrams_[(size_t)best];
        const float sc_orig = (paso == 0) ? sc_work : coseno_sdr_ordenado(q_orig, eg.sdr);
        const float req_work = (paso == 0) ? min_sim : (min_sim * 0.76f);
        const float req_orig = (paso == 0) ? min_sim : (min_sim * 0.70f);
        if (sc_work < req_work || sc_orig < req_orig) break;

        PremisaRecuperada pr;
        pr.id = best;
        pr.similitud = sc_orig;
        pr.macro_columna = eg.macro_columna;
        pr.texto = eg.texto;
        res.push_back(std::move(pr));
        excluidos.push_back(best);

        // Inhibicion lateral talamocortical: atenuar en `q_work` las asambleas ya explicadas
        // por la premisa actual para que el siguiente paso busque la pieza COMPLEMENTARIA.
        if (paso + 1 < top_k) {
            std::unordered_map<i32, float> inh_map;
            inh_map.reserve(eg.sdr.size() * 2);
            for (const auto& kv : eg.sdr) inh_map[kv.first] = kv.second;

            float nsq = 0.0f;
            for (auto& kv : q_work) {
                auto it = inh_map.find(kv.first);
                if (it != inh_map.end()) {
                    kv.second = std::max(0.0f, kv.second - inhibicion_lateral * it->second);
                }
                nsq += kv.second * kv.second;
            }
            if (nsq <= 1e-8f) break;
            const float inv = 1.0f / std::sqrt(nsq);
            for (auto& kv : q_work) kv.second *= inv;
        }
    }
    return res;
}

void CortezaSemanticaCSR::save(std::ostream& os) const {
    const uint32_t magic = 0x43584D31u; // "CXM1"
    wr_pod(os, magic);
    wr_pod(os, dim_);
    const uint32_t ne = (uint32_t)engrams_.size();
    wr_pod(os, ne);
    for (const auto& eg : engrams_) {
        const uint32_t len = (uint32_t)eg.texto.size();
        wr_pod(os, len);
        if (len > 0) os.write(eg.texto.data(), len);
        wr_pod(os, eg.macro_columna);
        wr_pod(os, eg.sorpresa_ach);
        wr_pod(os, eg.consolidaciones);
        const uint32_t ns = (uint32_t)eg.sdr.size();
        wr_pod(os, ns);
        if (ns > 0) {
            os.write(reinterpret_cast<const char*>(eg.sdr.data()),
                     (std::streamsize)(ns * sizeof(std::pair<i32, float>)));
        }
    }
}

void CortezaSemanticaCSR::load(std::istream& is) {
    uint32_t magic = 0;
    rd_pod(is, magic);
    if (magic != 0x43584D31u) throw std::runtime_error("formato CortezaSemanticaCSR invalido");
    rd_pod(is, dim_);
    uint32_t ne = 0;
    rd_pod(is, ne);
    engrams_.assign(ne, {});
    syn_inv_.assign((size_t)std::max(0, dim_), {});
    for (uint32_t i = 0; i < ne; ++i) {
        auto& eg = engrams_[i];
        uint32_t len = 0;
        rd_pod(is, len);
        eg.texto.resize(len);
        if (len > 0) is.read(&eg.texto[0], len);
        rd_pod(is, eg.macro_columna);
        rd_pod(is, eg.sorpresa_ach);
        rd_pod(is, eg.consolidaciones);
        uint32_t ns = 0;
        rd_pod(is, ns);
        eg.sdr.resize(ns);
        if (ns > 0) {
            is.read(reinterpret_cast<char*>(eg.sdr.data()),
                    (std::streamsize)(ns * sizeof(std::pair<i32, float>)));
        }
        for (const auto& kv : eg.sdr) {
            if (kv.first >= 0 && kv.first < dim_) {
                syn_inv_[(size_t)kv.first].push_back({(i32)i, kv.second});
            }
        }
    }
    score_buf_.assign(engrams_.size(), 0.0f);
    touched_.clear();
}

float FisiologiaViva::calcular_acetilcolina(float bpc_entrada, float max_resonancia_mem) {
    const float surp_norm = std::max(0.0f, std::min(1.0f, (bpc_entrada - 1.3f) / 3.2f));
    const float nov_mem   = std::max(0.0f, std::min(1.0f, 1.0f - max_resonancia_mem));
    return std::max(0.0f, std::min(1.0f, 0.45f * surp_norm + 0.55f * nov_mem));
}

bool FisiologiaViva::debe_aprender_autonomamente(const std::string& enunciado,
                                                 float ach,
                                                 float sim_ca3_actual) {
    if (enunciado.size() < 22 || enunciado.size() > 260) return false;
    if (enunciado[0] == '/') return false;
    if (enunciado.find('?') != std::string::npos) return false;
    // Verificar si empieza con "¿" en UTF-8 (0xC2 0xBF)
    if (enunciado.find("\xC2\xBF") != std::string::npos) return false;
    int espacios = 0;
    for (char c : enunciado) if (c == ' ') espacios++;
    if (espacios < 3) return false;
    return (ach >= 0.46f && sim_ca3_actual < 0.78f);
}

EstadoNeuromodulador FisiologiaViva::evaluar_certeza_ganglios(const double* probs_arranque,
                                                              int vocab_activo,
                                                              float sim_ca3,
                                                              float sim_semantica,
                                                              float ach_actual,
                                                              float umbral_go) {
    EstadoNeuromodulador est;
    est.acetilcolina_ach = ach_actual;

    double H = 0.0;
    double p_max = 0.0;
    for (int b = 0; b < 256; ++b) {
        const double p = probs_arranque[b];
        if (p > 1e-12) H -= p * std::log2(p);
        if (p > p_max) p_max = p;
    }
    const double H_max = std::log2((double)std::max(4, vocab_activo));
    const float nitidez_dorsal = (float)std::max(0.0, std::min(1.0, 1.0 - (H / std::max(1.0, H_max))));
    const float pico_prob      = (float)std::max(0.0, std::min(1.0, p_max));
    const float res_mem        = std::max(sim_ca3, sim_semantica);

    // La via directa (Go) requiere o bien resonancia solida en memoria (CA3 / Semantica CSR)
    // o bien alta convergencia de las asambleas dorsales en el arranque de deliberacion.
    const float certeza_mem    = std::min(1.0f, res_mem * 1.55f);
    float certeza_dorsal       = std::min(1.0f, 0.58f * nitidez_dorsal + 0.42f * pico_prob);
    // Via indirecta D2 (No-Go): si la pregunta dispara sorpresa alta (ACh > 0.85), la memoria
    // episodica/semantica tiene resonancia nula Y ademas la propia via dorsal presenta alta
    // dispersion entropica (H > 3.30 bits), los ganglios basales frenan la alucinacion:
    if (ach_actual > 0.85f && res_mem < 0.14f && H > 1.35) {
        certeza_dorsal *= std::max(0.18f, 1.0f - 4.5f * (ach_actual - 0.82f));
    }
    est.entropia_arranque = (float)H;
    est.certeza_ganglios  = std::max(certeza_mem, certeza_dorsal);
    est.compuerta_go      = (est.certeza_ganglios >= umbral_go);
    return est;
}

} // namespace cortex
