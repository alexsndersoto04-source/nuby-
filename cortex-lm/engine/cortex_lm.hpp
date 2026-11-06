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
#include <unordered_map>
#include <unordered_set>

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
    // 8 ticks por simbolo (antes 4). Medido: subir el tiempo de integracion es
    // la unica palanca de memoria que mejora por si sola (3,437 -> 3,327 bpc a
    // LH=1) y, combinada con theta_decay 0,95, baja el sistema completo de
    // 0,941 a 0,532 bpc (43 %). Ver OPCION-B-MEMORIA.md.
    int   ticks_per_char = 8;      // ticks por simbolo

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
    // 0,95 y no 0,80: la fatiga es lo que codifica el pasado, asi que relajarla
    // mas despacio la conserva. A 0,98 se pasa de freno y empeora (3,497 bpc).
    float theta_decay    = 0.95f;  // cuanto se relaja por tick
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
    // --- PASO 4: factor postsinaptico ---
    // dw = -eta * (senal local) * traza_pre * traza_post
    // La traza postsinaptica marca cuando disparo la neurona de DESTINO. Sin
    // ella la regla refuerza "lo que estaba activo cuando hubo error", no "lo
    // que hizo disparar a la neurona". Es el tercer factor que faltaba.
    float tau_post       = 12.0f;  // ticks de la traza postsinaptica
    float trace_post_w   = 1.0f;   // peso del factor post (0 = regla antigua)

    // --- PASO 5: senal de error propia de la capa ---
    // La capa de entrada tiene una prediccion top-down de que poblacion deberia
    // activarse (la que corresponde al simbolo actual, que es su propia
    // entrada). El error de prediccion es la diferencia entre esa prediccion y
    // los disparos que realmente ocurrieron. Es local, verificable y NO depende
    // del lector ni de ninguna matriz aleatoria.
    float pred_lr        = 0.02f;  // ritmo de la prediccion top-down
    float pred_gain      = 1.0f;   // cuanto pesa el error local (1 = el)
    float bias_homeo_lr  = 0.0f;   // homeostasis de umbral (Paso 6, mas adelante)

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

    // ======================================================================
    // MODULACION DOPAMINERGICA (tercer factor)
    // ======================================================================
    // dw = eta * STDP * D(t).  D(t) es una senal GLOBAL de refuerzo que solo
    // depende de lo que la red ACABA de escribir y de un diccionario estatico.
    // No viene de ningun gradiente: es una senal difusa, como la dopamina.
    //
    //    +1  si la ultima palabra formada existe en el diccionario
    //    -1  si no existe y ya es lo bastante larga como para ser una palabra
    //     0  mientras la palabra esta a medias
    //
    // La dopamina tiene su propia dinamica (sube de golpe, baja despacio) para
    // que el refuerzo alcance a las sinapsis que participaron, no solo a la
    // ultima que disparo. Sin esa cola, D(t) llegaria tarde a todas.
    // ACTIVADA por defecto: es la unica de las tres piezas con efecto medido
    // (3,67 frente a 3,74 bits/caracter en 3 semillas). Ver ACTA-TRES-PIEZAS.md.
    float dop_gain       = 1.0f;   // cuanto pesa D(t) en la regla
    float dop_rise       = 0.6f;   // subida por evento (0..1)
    float dop_tau        = 40.0f;  // ticks de caida (lento a proposito)
    int   dop_min_len    = 3;      // longitud a partir de la cual se juzga palabra
    std::vector<std::string> diccionario;   // palabras validas (minusculas)
    // Refuerzo tambien por estructura: una letra que casi nunca sigue a la
    // anterior delata ruido. Se mide con un bigrama fijo cargado del corpus.

    // ======================================================================
    // JERARQUIA TEMPORAL: poblacion rapida + poblacion lenta
    // ======================================================================
    // La capa se parte en dos poblaciones con constantes de membrana distintas:
    //   rapida (primeras n_fast): tau_m corto, sigue el ritmo de las letras
    //   lenta  (resto)          : tau_m largo, retiene el contexto de la frase
    // La lenta realimenta a la rapida por una matriz fija (no entrenada), con
    // retraso axonal largo guardado en un buffer circular por neurona.
    // DESACTIVADA por defecto: medido 3,76-3,77 frente a 3,73-3,74. Su estado se
    // actualiza pero el lector no lo aprovecha con este tamano de texto.
    int   n_fast         = 0;      // 0 = todo rapido (comportamiento anterior)
    float tau_m_fast     = 8.0f;
    float tau_m_slow     = 80.0f;
    float slow_to_fast   = 0.15f;  // ganancia de la realimentacion lenta->rapida
    int   slow_delay     = 24;     // ticks de retraso axonal de esa via
    int   slow_fanout    = 6;      // destinos por neurona lenta
    // Cuanto de la entrada sensorial llega tambien a la poblacion lenta. Sin
    // esto la memoria de trabajo esta sorda y su estado no codifica nada.
    float slow_in_gain    = 0.45f;

    // ======================================================================
    // OSCILADOR INTERNO DE CONTROL (estado de contexto)
    // ======================================================================
    // Un grupo pequeno de neuronas marcapasos dispara de forma ritmica y
    // autonoma (no necesita entrada). Su corriente entra como bias en la capa y
    // da a la red una "brujula" que cambia despacio dentro del bloque de texto.
    // Dos marcapasos con frecuencias distintas y no multiplas: su fase relativa
    // recorre muchos valores y codifica en que punto del bloque estamos.
    // DESACTIVADO por defecto: implementado y medido, pero EMPEORA. Con el motor
    // arreglado (el lector ya ve el estado) da 4,45 frente a 3,01-3,03. Anclar
    // la fase al simbolo lo empeora mas (6,04-6,16) y bajar la ganancia lo deja
    // en 3,49-3,54: nunca gana. Medido el mecanismo (tools/diag_estado.cpp): la
    // corriente sube la carga media de 0,065 a 0,335 SIN subir la varianza del
    // estado, o sea mas señal y cero informacion. Ver ACTA-TRES-PIEZAS-V2.md.
    // Se deja disponible para probar otra dinamica, no porque funcione.
    int   n_pace         = 0;      // numero de marcapasos (0 = desactivado)
    float pace_gain      = 0.35f;  // corriente que inyecta cada marcapasos
    float pace_period_a  = 11.0f;  // ticks del primer marcapasos
    float pace_period_b  = 29.0f;  // ticks del segundo
    float pace_bias_fast = 1.0f;   // peso del marcapasos sobre la poblacion rapida
    float pace_bias_slow = 0.6f;   // peso sobre la poblacion lenta
    // ANCLAJE AL SIMBOLO (medido y descartado tambien): con pace_lock las fases
    // vuelven a un punto fijo al cerrar cada simbolo, para que la corriente diga
    // "por aqui va el caracter" en vez de ser un reloj libre. Resultado: 6,04-6,16
    // frente a 3,01-3,03, o sea PEOR que el oscilador libre. Se conserva el
    // parametro para no repetir el experimento a ciegas.
    int   pace_lock        = 0;    // 1 = fases reiniciadas en cada simbolo
    float pace_phase_reset = 0.0f; // fase a la que vuelven (0 = principio)

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
    // Traza presinaptica LENTA para la via jerarquica: constante de tiempo
    // mucho mayor, de modo que la huella de una neurona lenta dure simbolos.
    std::vector<float> pre_trace_slow;
    float alpha_slow = 0.0f;
    // Traza postsinaptica (Paso 4): vale 1 en el tick en que la neurona de
    // destino disparo y decae con tau_post. Es el tercer factor de la regla.
    std::vector<float> post_trace;
    float alpha_post = 0.0f;        // factor de decaimiento por tick
    // Prediccion top-down por neurona (Paso 5): que actividad espera de si
    // misma esta neurona, dado el simbolo que la capa esta viendo.
    std::vector<float> pred;
    std::vector<float> pred_err;    // error de prediccion del ultimo simbolo
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
    // Features del ESTADO: potencial de membrana de cada neurona, normalizado.
    // Por que existe: theta_features solo expone la FATIGA. Con eso el lector
    // ve cuanto disparo cada neurona hace poco, pero no en que estado esta
    // ahora. Medido: el mismo lector llega a 2,94 bits/caracter cuando lee el
    // estado de un reservorio (bench_res) y se queda en 3,56 leyendo solo la
    // fatiga del motor. La diferencia no es la red: es lo que se le ensena.
    void state_features(std::vector<float>& out) const;
    // Las dos cosas juntas: estado + fatiga. Es lo que usa el banco bueno.
    void full_features(std::vector<float>& out) const;

    // MEMORIA DE LECTURA: la misma informacion que full_features, pero
    // repetida `n_tap` veces con retardo de un caracter cada vez (el actual, el
    // del caracter anterior, el de dos antes...). Medido con diag_var: el estado
    // de la red NO se acuerda del caracter anterior (correlacion -0,03), asi que
    // con una sola foto el lector solo puede aprender "que letra es esta" y su
    // techo es el bigrama. Con los taps ve la foto de antes y de antes de antes.
    void tap_features(std::vector<float>& out, int n_tap) const;
    // Prepara el historial con `n` fotos. Llamar antes de empezar a alimentar.
    void tap_hist_reserve(int n) {
        tap_hist_.assign(std::max(1, n),
                         std::vector<float>(p_.n_layers * p_.layer_size, 0.0f));
    }

    void readout(const std::vector<float>& f, float* logits) const;
    void normalize(const std::vector<float>& in, std::vector<float>& out) const;
    void update_norm(const std::vector<float>& f);
    void learn(const float* error, const std::vector<float>& f);
    // PASO 5: error de prediccion local de una capa (difiere prediccion y
    // disparos reales del ultimo simbolo, actualiza la prediccion y devuelve
    // el error por neurona).
    void error_local(int capa, std::vector<float>& out);
    // Estadisticas del error local: cuantas neuronas y cuanto.
    void stats_error_local(int capa, float& medio, float& maximo) const;

    // ===================== MODULACION DOPAMINERGICA =====================
    // Carga el diccionario (una linea por palabra) y los bigramas del corpus.
    void cargar_diccionario(const std::string& ruta);
    void cargar_bigramas(const std::string& ruta_corpus);
    // Juzga la palabra en curso y emite el pulso de dopamina (+1 / -1 / 0).
    // Devuelve el evento emitido. Se llama al cerrar cada simbolo.
    float juzgar_palabra(int ultimo_simbolo);
    float dopamina() const { return dop_; }
    long  dopamina_positivos() const { return dop_pos_; }
    long  dopamina_negativos() const { return dop_neg_; }
    const std::string& palabra_actual() const { return palabra_; }

    // ======================== JERARQUIA TEMPORAL ========================
    // Cuantas neuronas son rapidas y cuantas lentas (0,0 si esta desactivada).
    void poblaciones(int& rapidas, int& lentas) const {
        rapidas = p_.n_fast > 0 ? p_.n_fast : (int)layers_.empty() ? 0 : layers_[0].n;
        lentas  = p_.n_fast > 0 ? std::max(0, (int)layers_[0].n - p_.n_fast) : 0;
    }

    // ========================= OSCILADOR INTERNO =========================
    float oscilador(int i) const {
        return i < (int)pace_phase_.size() ? pace_phase_[i] : 0.0f;
    }

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
    int  ch_actual_ = -1;     // simbolo que se esta presentando (Paso 5)

    // --- dopamina (tercer factor) ---
    float dop_ = 0.0f;           // nivel actual de dopamina D(t)
    float dop_evento_ = 0.0f;    // ultimo evento (+1 refuerzo, -1 castigo)
    long  dop_pos_ = 0, dop_neg_ = 0;
    std::string palabra_;        // palabra que se esta formando
    std::unordered_set<std::string> dic_;   // diccionario en hash
    // bigramas del corpus: refuerzo/castigo por estructura aunque la palabra
    // sea nueva ("plabara" no esta en el diccionario pero sus bigramas si).
    std::unordered_map<uint32_t, float> bigr_;
    float bigr_umbral_ = 0.0f;

    // --- jerarquia temporal ---
    // Via lenta -> rapida: matriz fija dispersa y un buffer circular por
    // neurona rapida con el retraso axonal.
    std::vector<i32>   slow_src_;      // destinos rapidos x fanout
    std::vector<float> slow_w_;
    std::vector<std::vector<float>> slow_buf_;
    int slow_buf_len_ = 0;

    // --- memoria de lectura (ver tap_features) ---
    // tap_hist_[0] = estado al cerrar el simbolo actual, [1] = el anterior...
    mutable std::vector<std::vector<float>> tap_hist_;

    // --- oscilador ---
    std::vector<float> pace_phase_;    // fase de cada marcapasos
    std::vector<float> pace_period_;
    float inh_prev_ = 0.0f;   // actividad reciente que alimenta el freno
    long spikes_ = 0;
    long symbols_ = 0;
    long last_consol_ = 0;
    std::mt19937 rng_;
    std::vector<std::pair<float, i32>> cand_;

    void build_sensory_map();
};

}  // namespace cortex
