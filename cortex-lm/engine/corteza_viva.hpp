#pragma once
// corteza_viva.hpp — Organos cognitivos vivos y escalables de CORTEX-LM Macro:
//   1. CortezaSemanticaCSR ("Leer = Saber"): grafo sinaptico invertido O(activos)
//      con inhibicion lateral entre premisas para sintesis multi-documento.
//   2. FisiologiaViva: neuromodulacion autonoma por Acetilcolina (ACh, sorpresa),
//      compuerta de honestidad tipo Ganglios Basales (Go / No-Go) sin alucinaciones
//      y memoria autobiografica continua de conversacion.
//   3. Consolidacion por Sueno Cortical de Ondas Lentas (replay hipocampo/semantica -> corteza).
// Todo 100% impulsado por asambleas de neuronas pulsantes LIF (SDR NMDA), cero backprop.

#include "cortex_lm.hpp"
#include <functional>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace cortex {

struct EngramaSemantico {
    std::string texto;
    int32_t     macro_columna = -1;
    float       sorpresa_ach  = 0.5f;
    uint32_t    consolidaciones = 0;
    std::vector<std::pair<i32, float>> sdr; // huella cortical normalizada L2
};

struct PremisaRecuperada {
    int32_t     id = -1;
    float       similitud = 0.0f;
    int32_t     macro_columna = -1;
    std::string texto;
};

// Corteza Semantica Entorrinal-Neocortical con indice sinaptico invertido disperso:
// permite almacenar decenas o cientos de miles de conceptos leidos en modo --continuo
// y recuperar en <1 ms multiples premisas COMPLEMENTARIAS mediante inhibicion lateral.
class CortezaSemanticaCSR {
public:
    void init(int concept_dim);
    int  concept_dim() const { return dim_; }
    size_t num_engrams() const { return engrams_.size(); }
    const EngramaSemantico& engram(size_t idx) const { return engrams_[idx]; }
    EngramaSemantico& engram_mut(size_t idx) { return engrams_[idx]; }

    // Registra un engrama si su huella SDR aporta informacion genuinamente nueva (sim < max_dup_sim).
    // Devuelve el id del engrama (>=0) o -1 si ya estaba representado.
    int asimilar_pasaje_sdr(const std::vector<std::pair<i32, float>>& sdr,
                            const std::string& texto_limpio,
                            int macro_col = -1,
                            float sorpresa_ach = 0.5f,
                            float max_dup_sim = 0.90f);

    // Extrae pasajes informativos de un documento leido en modo continuo, los pasa por
    // las neuronas LIF de `eng` y cablea sus huellas SDR en el indice invertido.
    // Devuelve cuantos engramas semanticos nuevos se consolidaron.
    size_t asimilar_documento_vivo(Engine& eng,
                                   const std::string& documento,
                                   const std::function<float(const std::string&)>& medidor_bpc = nullptr,
                                   size_t max_nuevos = 0);

    // Recupera hasta `top_k` premisas complementarias aplicando inhibicion lateral
    // sobre las neuronas conceptuales ya explicadas por cada premisa ganadora.
    std::vector<PremisaRecuperada> recuperar_premisas(
        const std::vector<std::pair<i32, float>>& query_sdr,
        int top_k = 2,
        float min_sim = 0.21f,
        float inhibicion_lateral = 0.70f) const;

    void save(std::ostream& os) const;
    void load(std::istream& is);

private:
    int dim_ = 0;
    std::vector<EngramaSemantico> engrams_;
    std::vector<std::vector<std::pair<i32, float>>> syn_inv_; // neurona_j -> [(engram_id, peso)]
    mutable std::vector<float> score_buf_;
    mutable std::vector<i32>   touched_;

    int buscar_mejor_interno(const std::vector<std::pair<i32, float>>& q_sdr,
                             const std::vector<int>& excluidos,
                             float& best_score) const;
};

struct EstadoNeuromodulador {
    float acetilcolina_ach = 0.0f; // [0..1] sorpresa / apertura de plasticidad viva
    float certeza_ganglios = 1.0f; // [0..1] concentracion + resonancia cortical
    float entropia_arranque = 0.0f;// bits de incertidumbre en el arranque dorsal
    bool  compuerta_go     = true; // true = hablar con fundamento, false = abstencion honesta
    bool  aprendio_en_vivo = false;// true = consolido automaticamente el enunciado en CA3
};

class FisiologiaViva {
public:
    // Calcula el nivel de Acetilcolina (ACh) a partir de la sorpresa predictiva (bits/byte)
    // y la novedad frente a las memorias existentes.
    static float calcular_acetilcolina(float bpc_entrada, float max_resonancia_mem);

    // Decide si un enunciado conversacional del usuario debe consolidarse autonomamente
    // en el hipocampo CA3 por alta sorpresa ACh y estructura declarativa (sin '?' ni comando).
    static bool debe_aprender_autonomamente(const std::string& enunciado,
                                            float ach,
                                            float sim_ca3_actual);

    // Compuerta Go / No-Go de los Ganglios Basales: combina la nitidez de la distribucion
    // de arranque dorsal (1 - H/H_max) con la resonancia en CA3 y CortezaSemanticaCSR.
    static EstadoNeuromodulador evaluar_certeza_ganglios(const double* probs_arranque,
                                                         int vocab_activo,
                                                         float sim_ca3,
                                                         float sim_semantica,
                                                         float ach_actual,
                                                         float umbral_go = 0.23f);

    // Segmenta un texto largo en enunciados/ideas limpias aptas para indexacion semantica.
    static std::vector<std::string> extraer_enunciados_informativos(const std::string& doc);
};

} // namespace cortex
