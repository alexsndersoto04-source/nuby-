// bpe.cpp — parte el texto en trozos (BPE) y deja los ficheros listos.
//
// POR QUE
// -------
// Todo lo medido hasta ahora predice LETRAS. A ese nivel, un contador que mira
// 3 letras es un rival durisimo: casi todo esta en esas 3 letras y la red solo
// puede arañar 0,036 bpc. Prediciendo TROZOS (pedazos de palabra: " que",
// "cion", "mente") el contexto util se reparte en mas simbolos y la red tiene
// sitio para aportar algo que el contador no tenga.
//
// Se hace BPE de verdad: se empieza con bytes y se fusionan los pares mas
// frecuentes. Cada fusion crea un simbolo nuevo. El texto se guarda como
// enteros de 32 bits para no depender de la codificacion.
//
// Uso:
//   bpe FUSIONES [MIN_FREQ]
//   bpe 6000 3
//
// Salida: data/train.ids, data/hold.ids, data/bpe.txt (alfabeto + fusiones).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <unordered_map>
#include <vector>

static std::vector<uint32_t> carga(const char *p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { std::fprintf(stderr, "no se puede abrir %s\n", p); std::exit(1); }
    std::vector<uint32_t> v;
    char c;
    while (in.get(c)) v.push_back((uint32_t)(unsigned char)c);
    return v;
}

static void guarda(const char *p, const std::vector<uint32_t> &v) {
    std::ofstream out(p, std::ios::binary);
    for (uint32_t x : v) {
        const char b[4] = {(char)(x & 0xff), (char)((x >> 8) & 0xff),
                           (char)((x >> 16) & 0xff), (char)((x >> 24) & 0xff)};
        out.write(b, 4);
    }
}

int main(int argc, char **argv) {
    const int FUS = argc > 1 ? std::atoi(argv[1]) : 6000;
    const int MIN = argc > 2 ? std::atoi(argv[2]) : 3;

    auto tr = carga("data/real_train.txt");
    auto te = carga("data/real_hold.txt");
    std::printf("train %zu bytes | test %zu bytes | fusiones pedidas %d\n",
                tr.size(), te.size(), FUS);

    // tabla de pares: clave = (a << 20) | b  (los simbolos llegan a < 1e6)
    // CUIDADO: la clave se calcula en 64 bits. Con 32 la suma desborda y pares
    // distintos colisionan (paso: 2.000 fusiones seguidas del mismo par falso).
    auto clave = [](uint32_t ca, uint32_t cb) -> uint64_t {
        return ((uint64_t)ca << 32) | (uint64_t)cb;
    };
    std::unordered_map<uint64_t, int> pares;
    auto cuenta = [&](const std::vector<uint32_t> &v) {
        pares.clear();
        pares.reserve(v.size());
        for (size_t i = 0; i + 1 < v.size(); ++i) pares[clave(v[i], v[i + 1])]++;
    };
    // a, b: VALORES a fusionar (no indices).
    auto aplica = [&](std::vector<uint32_t> v, uint32_t sv1, uint32_t sv2,
                      uint32_t nuevo) {
        const uint32_t a = sv1, b = sv2;
        std::vector<uint32_t> out;
        out.reserve(v.size());
        for (size_t i = 0; i < v.size();) {
            if (i + 1 < v.size() && v[i] == a && v[i + 1] == b) {
                out.push_back(nuevo);
                i += 2;
            } else {
                out.push_back(v[i]);
                ++i;
            }
        }
        return out;
    };

    std::vector<std::pair<std::pair<uint32_t, uint32_t>, uint32_t>> fus;
    uint32_t siguiente = 256;
    cuenta(tr);
    {
        std::vector<std::pair<int,uint64_t>> top;
        for (auto &kv : pares) top.push_back({kv.second, kv.first});
        std::sort(top.rbegin(), top.rend());
        std::printf("  [dep] pares distintos %zu; los 3 mas frecuentes:\n", pares.size());
        for (int i = 0; i < 3 && i < (int)top.size(); ++i)
            std::printf("        clave=0x%llx n=%d -> a=%llu b=%llu\n",
                        (unsigned long long)top[i].second, top[i].first,
                        (unsigned long long)(top[i].second >> 32),
                        (unsigned long long)(top[i].second & 0xffffffffu));
    }
    for (int k = 0; k < FUS; ++k) {
        // el par mas frecuente
        uint64_t mejor = 0;
        int mejor_n = 0;
        for (auto &kv : pares)
            if (kv.second > mejor_n) { mejor_n = kv.second; mejor = kv.first; }
        if (mejor_n < MIN) {
            std::printf("  paro: el par mas frecuente solo sale %d veces (< %d)\n",
                        mejor_n, MIN);
            break;
        }
        const uint32_t va = (uint32_t)(mejor >> 32);
        const uint32_t vb = (uint32_t)(mejor & 0xffffffffu);
        if (k < 6) {
            long cnt_antes = 0;
            for (size_t i = 0; i + 1 < tr.size(); ++i)
                if (tr[i] == va && tr[i + 1] == vb) cnt_antes++;
            std::printf("  [dep] k=%d par=(%u,%u) n=%d | en el texto: %ld\n",
                        k, va, vb, mejor_n, cnt_antes);
        }
        fus.push_back({{va, vb}, siguiente});
        tr = aplica(tr, va, vb, siguiente);
        ++siguiente;
        cuenta(tr);
        if (k < 6) std::printf("  [dep] tras fusionar: %zu pares distintos\n",
                               pares.size());
        if (k % 200 == 0)
            std::printf("  fusion %d: (%u,%u) -> %u | quedan %zu simbolos\n",
                        k + 1, va, vb, siguiente - 1, tr.size());
    }
    // aplicar al test en el mismo orden
    for (auto &f : fus) te = aplica(te, f.first.first, f.first.second, f.second);

    std::printf("hechas %zu fusiones | alfabeto %u | train %zu piezas | "
                "test %zu piezas\n", fus.size(), siguiente, tr.size(), te.size());
    std::printf("compresion: %.2f bytes por pieza\n",
                (double)te.size() * 0 + (double)254096 / (double)tr.size());
    guarda("data/train.ids", tr);
    guarda("data/hold.ids", te);
    {
        std::ofstream o("data/bpe.txt");
        o << siguiente << "\n";
        for (auto &f : fus) o << f.first.first << " " << f.first.second << "\n";
    }
    std::printf("escrito data/train.ids, data/hold.ids, data/bpe.txt\n");
    return 0;
}
