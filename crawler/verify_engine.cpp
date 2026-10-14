// ============================================================================
//  VERIFICACIÓN: el motor C++ de Nuby carga el índice recién rastreado.
//  Compila el MISMO SearchIndex que usa el navegador (nativo y wasm) contra el
//  TSV producido por crawler/build_index.py y muestra números reales.
//
//  Uso:  ./verify_engine nuby_engine/data/crawl_pages.tsv "tu consulta"
// ============================================================================
#include "../nuby_engine/include/nuby/search/search_index.hpp"
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "uso: %s <crawl_pages.tsv> [consulta]\n", argv[0]); return 2; }
    nuby::search::SearchIndex index;
    size_t n = index.load(argv[1]);
    std::printf("[motor] documentos cargados del TSV real : %zu\n", n);

    std::string query = argc > 2 ? argv[2] : "inteligencia artificial";
    auto hits = index.query(query, 5);
    std::printf("[motor] consulta \"%s\" → %zu resultados\n", query.c_str(), hits.size());
    int shown = 0;
    for (const auto& h : hits) {
        std::printf("   %.3f  %s  [%s]\n", h.score, h.doc->title.substr(0, 70).c_str(),
                    h.doc->domain.c_str());
        if (++shown >= 5) break;
    }
    if (hits.empty()) { std::fprintf(stderr, "FALLO: el motor no encontró nada\n"); return 1; }
    return 0;
}
