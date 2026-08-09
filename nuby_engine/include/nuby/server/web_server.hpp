#pragma once

#include <string>
#include <thread>
#include <memory>
#include <atomic>
#include "../app/browser_shell.hpp"

namespace nuby::server {

// Servidor HTTP del navegador Nuby.
// Su única misión es TRANSPORTE: entrega los píxeles que el motor rasterizó
// (comprimidos con RLE propio) y recibe eventos de entrada (clicks, teclas,
// rueda). No contiene ni una sola etiqueta de la interfaz del navegador:
// toda la UI la pinta el motor.
class WebServer {
public:
    WebServer(int port, const std::string& host) : port_(port), host_(host) {
        shell_ = std::make_shared<app::BrowserShell>();
    }

    std::shared_ptr<app::BrowserShell> shell() const { return shell_; }

    void run_synchronous();
    void start();
    void stop();

private:
    void handle_client(int client_sock);
    std::string handle_request(const std::string& method, const std::string& path,
                               const std::string& query, const std::string& body,
                               bool& is_binary);

    int port_;
    std::string host_;
    bool running_{false};
    std::unique_ptr<std::thread> server_thread_;
    std::shared_ptr<app::BrowserShell> shell_;
    std::atomic<unsigned long> frame_seq_{0};
};

} // namespace nuby::server
