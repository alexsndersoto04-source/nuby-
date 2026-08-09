// ============================================================================
// NUBY 2.0 — Navegador cuya interfaz completa la pinta su propio motor.
// Arranque: carga el índice de búsqueda (datos de rastreo real) y sirve
// los píxeles del motor por HTTP. Sin teatro.
// ============================================================================

#include "../include/nuby/nuby_engine.hpp"
#include "../include/nuby/server/web_server.hpp"
#include <iostream>
#include <chrono>
#include <filesystem>
#include <cstdlib>

int main(int argc, char* argv[]) {
    std::cout << "\033[1;37m"
              << "======================================================================\n"
              << "   NUBY 2.0 — el navegador se pinta a si mismo\n"
              << "   HTML/CSS/Layout/Raster/JS/BM25: todo del motor, todo real\n"
              << "======================================================================\n"
              << "\033[0m";

    int port = 8080;
    // Render/Fly/Koyeb asignan el puerto vía variable de entorno PORT
    if (const char* env = std::getenv("PORT"); env && *env) {
        try { port = std::stoi(env); } catch (...) { /* PORT inválido: queda 8080 */ }
    }
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--port") port = std::stoi(argv[i + 1]);
    }

    nuby::server::WebServer server(port, "0.0.0.0");

    // Carga del índice REAL (exportado del rastreo de spider.py)
    std::string data_path = "data/crawl_pages.tsv";
    if (!std::filesystem::exists(data_path)) data_path = "../data/crawl_pages.tsv";
    auto t0 = std::chrono::steady_clock::now();
    size_t loaded = server.shell()->index().load(data_path);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    server.shell()->set_data_path(data_path);

    std::cout << "[indice] " << loaded << " documentos reales cargados de " << data_path
              << " en " << ms << " ms\n"
              << "[indice] " << server.shell()->index().term_count()
              << " terminos unicos en el indice invertido\n" << std::flush;
    server.shell()->reload_home(); // la home ya muestra los números reales

    // Auto-test real del pipeline (medición verdadera, sin números guionizados)
    auto res = nuby::NubyBrowserEngine(800, 600).render_page(
        "<div style=\"background:#111;padding:20px;\">"
        "<h1 style=\"color:#38bdf8;\">Self-test Nuby</h1></div>");
    std::cout << "[pipeline] ciclo de render medido: "
              << (res.profiler.total_duration_us() / 1000.0) << " ms (real)\n"
              << "----------------------------------------------------------------------\n";

    server.run_synchronous();
    return 0;
}
