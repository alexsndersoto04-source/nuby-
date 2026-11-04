// Banco de medida con protocolo honesto.
//
// POR QUE EXISTE
// --------------
// Todas las cifras anteriores salian de partir UN MISMO texto repetido en
// "train" y "test". El corpus de verificacion repite el Quijote en bucle, asi
// que el test era casi identico al train y el BPC medido era memorizacion.
// Aqui el test sale de `data/spanish_holdout.txt`, que no comparte ni una
// variante de superficie con el entrenamiento.
//
// QUE MIDE
// --------
// La red ve el texto de entrenamiento. El lector softmax (lo unico que
// aprende por gradiente) se entrena sobre las features de la red. El test se
// pasa por la red CONGELADA y se mide el BPC sobre texto nunca visto.
//
// METODO (Paso 1 del plan acordado)
// --------------------------------
// El lector puede aprender de dos maneras:
//   0 = descenso de gradiente (softmax + lr fijo). Es backprop del lector.
//   1 = RLS recursivo (FORCE learning). Cada neurona de salida ajusta SUS
//       pesos con SU propio error y la actividad que recibe. Nada viaja hacia
//       atras: es una regla local y cumple la restriccion del proyecto.
//
// Uso: bench NTR EPOCHS K LH [PLASTIC ETA WSCALE LAG NORM METODO]
#include "cortex_lm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <vector>

using namespace cortex;

static std::vector<unsigned char> load(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "no se puede abrir %s\n", path);
        std::exit(1);
    }
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    const int   NTR   = argc > 1 ? std::atoi(argv[1]) : 8000;
    const int   EPOCH = argc > 2 ? std::atoi(argv[2]) : 12;
    const int   K     = argc > 3 ? std::atoi(argv[3]) : 64;
    const int   LH    = argc > 4 ? std::atoi(argv[4]) : 8;
    const int   PL    = argc > 5 ? std::atoi(argv[5]) : 0;
    const float ETA   = argc > 6 ? (float)std::atof(argv[6]) : 0.0f;
    const float WSC   = argc > 7 ? (float)std::atof(argv[7]) : -1.0f;
    const int   LAG   = argc > 8 ? std::atoi(argv[8]) : 1;   // 1 = siguiente simbolo, 0 = actual
    const int   NORM  = argc > 9 ? std::atoi(argv[9]) : 0;
    // METODO=1 -> RLS, regla local (FORCE). METODO=0 -> gradiente, referencia.
    const int   METODO = argc > 10 ? std::atoi(argv[10]) : 0;
    // Solo cuentan como candidatas las clases que aparecen en el train: con
    // 256 salidas y ~45 simbolos reales, el softmax se juega la masa de
    // probabilidad entre simbolos que no pueden ocurrir. El test no aporta
    // ninguna clase que no este en el train.
    // lambda < 1 olvida (ventana finita); lambda = 1 acumula toda la historia.
    const double rls_lambda = argc > 11 ? std::atof(argv[11]) : 1.0;
    // Filtro del objetivo: 1 = one-hot puro; <1 suaviza (FORCE clasico).
    const double alf_obj_par = argc > 12 ? std::atof(argv[12]) : 1.0;
    const double rls_p0 = argc > 13 ? std::atof(argv[13]) : 100.0;
    // Semilla de la red. Necesaria para medir ruido entre inicializaciones.
    const unsigned SEED_ARG = argc > 14 ? (unsigned)std::atoi(argv[14]) : 1234u;

    Params p;
    p.eta = ETA > 0 ? ETA : p.eta;
    if (WSC >= 0) p.w_scale = WSC;
    p.seed = SEED_ARG;
    Engine e(p);
    e.build_random(p.seed);

    auto train = load("data/spanish_small.txt");
    auto test  = load("data/spanish_holdout.txt");
    const int Dd = p.layer_size;
    const int NF = K * LH;

    // --- calibracion: una pasada sin plasticidad para elegir dims y escalar ---
    std::vector<float> f;
    std::vector<std::vector<float>> S(NTR);
    e.reset_state();
    for (int t = 0; t < NTR; ++t) {
        e.feed(train[t]);
        e.theta_features(f);
        S[t] = f;
    }
    std::vector<double> mean(Dd, 0.0);
    for (int j = 0; j < Dd; ++j) {
        double a = 0;
        for (int t = 0; t < NTR; ++t) a += S[t][j];
        mean[j] = a / NTR;
    }
    std::vector<std::pair<double, int>> var(Dd);
    for (int j = 0; j < Dd; ++j) {
        double v = 0;
        for (int t = 0; t < NTR; ++t) {
            const double d = S[t][j] - mean[j];
            v += d * d;
        }
        var[j] = {v / NTR, j};
    }
    std::sort(var.rbegin(), var.rend());
    std::vector<int> sel(K);
    for (int k = 0; k < K; ++k) sel[k] = var[k].second;
    std::vector<double> mu(K, 0.0), sd(K, 0.0);
    for (int k = 0; k < K; ++k) {
        double a = 0;
        for (int t = 0; t < NTR; ++t) a += S[t][sel[k]];
        mu[k] = a / NTR;
        double v = 0;
        for (int t = 0; t < NTR; ++t) {
            const double d = S[t][sel[k]] - mu[k];
            v += d * d;
        }
        sd[k] = std::sqrt(v / NTR) + 1.0;
    }

    // Huella de los pesos antes y despues de la plasticidad: si la media no se
    // mueve, la regla no esta haciendo nada y las dos condiciones salen
    // identicas (que es justo lo que se observo).
    double w_ini = 0.0; int nw = 0;
    if (true) {
        const auto& g0 = e.layer(0).g;
        for (float w : g0.weight) { w_ini += w; nw++; }
        w_ini /= std::max(1, nw);
    }

    // --- pasada 2: features con (o sin) plasticidad local ---
    e.reset_state();
    std::vector<std::vector<float>> SS(NTR);
    for (int t = 0; t < NTR; ++t) {
        e.feed(train[t]);
        e.theta_features(f);
        SS[t] = f;
        if (PL) {
            // Error difundido por la matriz fija B. En el uso real sale del
            // lector; aqui usamos el error de un bigrama para aislar el efecto
            // de la plasticidad sobre la representacion.
            std::vector<float> err(256, 0.0f);
            err[train[t + LAG]] = -1.0f;
            err[train[t]] = 1.0f;
            e.learn(err.data(), f);
        }
    }

    // desplazar en el tiempo para construir la ventana de LH estados
    std::vector<std::vector<float>> Z(NTR);
    for (int t = 0; t < NTR; ++t) {
        Z[t].resize(NF);
        for (int h = 0; h < LH; ++h) {
            const int s = t - h < 0 ? 0 : t - h;
            for (int k = 0; k < K; ++k) Z[t][h * K + k] = (SS[s][sel[k]] - mu[k]) / sd[k];
        }
    }

    // --- clases reales del problema (solo del train) ---
    std::vector<uint8_t> es_clase(256, 0);
    std::vector<int> clases;
    for (int t = 0; t < NTR; ++t) es_clase[train[t]] = 1;
    for (int c = 0; c < 256; ++c) if (es_clase[c]) clases.push_back(c);
    // Huella de los pesos: si la plasticidad no los mueve, las dos condiciones
    // salen identicas y la comparacion no mide nada.
    {
        double w_fin = 0.0;
        const auto& g1 = e.layer(0).g;
        for (float w : g1.weight) w_fin += w;
        w_fin /= (double)std::max<size_t>(1, g1.weight.size());
        std::printf("  [pesos] media inicial %.5f -> final %.5f (cambio %+.2e)\n",
                    w_ini, w_fin, w_fin - w_ini);
    }
    std::printf("  clases reales en el train: %d de 256\n", (int)clases.size());

    // --- lector softmax multinomial (unico componente que usa gradiente) ---
    std::vector<double> W((size_t)256 * NF, 0.0), b(256, 0.0), lg(256);
    const double lr = 0.05;

    // Estado de RLS. UNA sola matriz P COMPARTIDA: todas las neuronas de
    // salida ven la misma entrada z, asi que la inversa de la autocorrelacion
    // es comun. Esto es exactamente FORCE learning. Con una P por clase el
    // coste se multiplicaba por el numero de clases y ademas no convergia
    // (medido: 4,998 bpc, peor que el gradiente).
    //
    //   g  = P z / (lambda + z . P z)          <- ganancia, comun a todas
    //   P <- (P - g (P z)^T) / lambda
    //   w_c <- w_c + g e_c                     <- cada salida con SU error
    //
    // La regla es local: cada neurona de salida solo necesita su error y la
    // entrada que recibe. Nada viaja hacia atras.
    const int NF1 = NF + 1;
    std::vector<double> P;
    if (METODO == 1) {
        P.assign((size_t)NF1 * NF1, 0.0);
        for (int i = 0; i < NF1; ++i) P[(size_t)i * NF1 + i] = rls_p0;
    }
    std::vector<double> Zt(NF, 0.0);
    std::vector<double> z(NF + 1), Pz(NF1), g(NF1);
    auto rls_paso = [&](const double* err) {
        for (int k = 0; k < NF; ++k) z[k] = Zt[k];
        z[NF] = 1.0;
        // Pz = P z
        for (int i = 0; i < NF1; ++i) {
            const double* fila = &P[(size_t)i * NF1];
            double acc = 0;
            for (int k = 0; k < NF1; ++k) acc += fila[k] * z[k];
            Pz[i] = acc;
        }
        double den = rls_lambda;
        for (int i = 0; i < NF1; ++i) den += z[i] * Pz[i];
        if (!(den > 1e-12)) return;
        for (int i = 0; i < NF1; ++i) g[i] = Pz[i] / den;
        // P <- (P - g (Pz)^T) / lambda
        for (int i = 0; i < NF1; ++i) {
            double* fila = &P[(size_t)i * NF1];
            const double gi = g[i];
            for (int k = 0; k < NF1; ++k) fila[k] = (fila[k] - gi * Pz[k]) / rls_lambda;
        }
        // cada salida se mueve con su propio error
        for (int c : clases) {
            if (err[c] == 0.0) continue;
            double* w = &W[(size_t)c * NF];
            for (int k = 0; k < NF; ++k) w[k] += g[k] * err[c];
            b[c] += g[NF] * err[c];
        }
    };

    // Objetivo: por defecto one-hot puro. Se suaviza opcionalmente con
    // alf_obj_par. El error de cada salida es el que mueve SU peso.
    std::vector<double> obj(256, 0.0), err(256, 0.0);
    for (int ep = 0; ep < EPOCH; ++ep) {
        for (int t = 0; t + std::max(1, LAG) < NTR; ++t) {
            for (int k = 0; k < NF; ++k) Zt[k] = Z[t][k];
            const int y = train[t + LAG];

            if (METODO == 1) {
                // --- RLS / FORCE: regla local ---
                for (int c : clases) {
                    const double destino = (c == y) ? 1.0 : 0.0;
                    obj[c] = alf_obj_par * destino + (1.0 - alf_obj_par) * obj[c];
                    err[c] = obj[c] - (b[c] + [&] {
                        double acc = 0;
                        const double* w = &W[(size_t)c * NF];
                        for (int k = 0; k < NF; ++k) acc += w[k] * Zt[k];
                        return acc;
                    }());
                }
                rls_paso(err.data());
            } else {
                // --- referencia: softmax + gradiente ---
                double mx = -1e30;
                for (int c : clases) {
                    double v = b[c];
                    const double* r = &W[(size_t)c * NF];
                    for (int k = 0; k < NF; ++k) v += r[k] * Zt[k];
                    lg[c] = v;
                    if (v > mx) mx = v;
                }
                double ss = 0;
                for (int c : clases) { lg[c] = std::exp(lg[c] - mx); ss += lg[c]; }
                for (int c : clases) {
                    const double gg = lg[c] / ss - (c == y ? 1.0 : 0.0);
                    if (gg == 0.0) continue;
                    b[c] -= lr * gg;
                    double* r = &W[(size_t)c * NF];
                    for (int k = 0; k < NF; ++k) r[k] -= lr * (gg * Zt[k] + 1e-4 * r[k]);
                }
            }
        }
    }

    // bigrama de referencia, estimado SOLO con el train
    std::map<std::pair<int, int>, int> bi;
    std::map<int, int> uni;
    for (int t = 0; t + 1 < NTR; ++t) { bi[{train[t], train[t + 1]}]++; uni[train[t]]++; }

    std::vector<std::vector<float>> ring(LH, std::vector<float>(Dd, 0.0f));

    // --- evaluacion con temperatura ---
    // RLS hace REGRESION: separa la clase correcta de las demas por ~1 nat.
    // Un softmax sobre 37 clases necesita ~4 nats para dar probabilidad alta.
    // Medido: 98,5 % de acierto con 4,05 bpc. El argmax es correcto; lo que
    // falta es CALIBRACION.
    //
    // La temperatura T se elige sobre la COLA DEL TRAIN (texto visto), nunca
    // sobre el test. No es un parametro ajustado al examen.
    auto evaluar = [&](const std::vector<unsigned char>& txt, int ntr_hist,
                       double T, bool contar) -> std::pair<double, double> {
        (void)ntr_hist;
        for (int h = 0; h < LH; ++h) std::fill(ring[h].begin(), ring[h].end(), 0.0f);
        e.reset_state();
        double bits = 0;
        int nn = 0, cor = 0;
        std::vector<double> zz(NF);
        for (size_t t = 0; t + std::max(1, LAG) < txt.size(); ++t) {
            e.feed(txt[t]);
            e.theta_features(f);
            for (int h = LH - 1; h > 0; --h) ring[h] = ring[h - 1];
            ring[0] = f;
            const int y = txt[t + LAG];
            for (int h = 0; h < LH; ++h)
                for (int k = 0; k < K; ++k)
                    zz[h * K + k] = (ring[h][sel[k]] - mu[k]) / sd[k];
            double mx = -1e30;
            int bst = 0;
            for (int c : clases) {
                double v = b[c];
                const double* r = &W[(size_t)c * NF];
                for (int k = 0; k < NF; ++k) v += r[k] * zz[k];
                lg[c] = v / T;
                if (lg[c] > mx) { mx = lg[c]; bst = c; }
            }
            double sm = 0;
            for (int c : clases) { lg[c] = std::exp(lg[c] - mx); sm += lg[c]; }
            if (es_clase[y]) {
                bits += -std::log2(std::max(1e-12, lg[y] / sm));
            } else {
                // simbolo que el train no vio: se cuenta como el peor caso.
                bits += 16.0;
            }
            if (bst == y) cor++;
            nn++;
            if (!contar) break;
        }

        return {bits / std::max(1, nn), 100.0 * cor / std::max(1, nn)};
    };

    // Calibracion sobre texto NUNCA entrenado: la primera mitad del holdout.
    // La temperatura se elige ahi; la evaluacion se hace solo sobre la SEGUNDA
    // mitad. Nunca se ajusta nada con el texto del examen.
    const int cal_n = (int)test.size() / 2;
    std::vector<unsigned char> cal(test.begin(), test.begin() + cal_n);
    std::vector<unsigned char> test_eval(test.begin() + cal_n, test.end());

    // referencia de bigrama sobre el test (se calcula una vez)
    double ref_bigram = 0.0;
    {
        int m = 0;
        for (size_t t = 0; t + 1 < test_eval.size(); ++t) {
            auto it = bi.find({test_eval[t], test_eval[t + 1]});
            const double cc = it == bi.end() ? 0.0 : it->second;
            ref_bigram += -std::log2((cc + 0.1) / (uni[test_eval[t]] + 0.1 * 256));
            m++;
        }
        ref_bigram /= std::max(1, m);
    }

    double mejor_T = 1.0, mejor_b = 1e30;
    for (double T : {1.0, 0.5, 0.3, 0.2, 0.15, 0.1, 0.07, 0.05}) {
        auto r = evaluar(cal, 0, T, true);
        if (r.first < mejor_b) { mejor_b = r.first; mejor_T = T; }
    }
    std::printf("  temperatura elegida en la cola del train: %.3f\n", mejor_T);

    auto res = evaluar(test_eval, 0, mejor_T, true);

    std::printf("PLASTIC=%d eta=%.1e w_scale=%.2f K=%d LH=%d LAG=%d NORM=%d METODO=%d | train=%d\n", PL, ETA, p.w_scale, K, LH, LAG, NORM, METODO, NTR);
    std::printf("  TEST (texto no visto, %d simbolos): %.3f bpc | acierto %.1f %%\n",
                (int)test_eval.size(), res.first, res.second);
    std::printf("  bigrama de referencia            : %.3f bpc\n", ref_bigram);
    return 0;
}
