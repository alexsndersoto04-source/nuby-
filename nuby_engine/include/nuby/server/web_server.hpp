#pragma once

#include <string>
#include <thread>
#include <memory>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <random>
#include <mutex>
#include "../app/browser_shell.hpp"

namespace nuby::server {

// Servidor HTTP del navegador Nuby.
// Su única misión es TRANSPORTE: entrega los píxeles que el motor rasterizó
// (comprimidos con RLE propio) y recibe eventos de entrada (clicks, teclas,
// rueda). No contiene ni una sola etiqueta de la interfaz del navegador:
// toda la UI la pinta el motor.
//
// MULTI-SESIÓN REAL (2026-08-09): cada visitante recibe una cookie
// `nuby_sid` y su PROPIO BrowserShell (historial, página actual, foco y
// scroll aislados). Las sesiones comparten un único SearchIndex en RAM
// (buscador colectivo, protegido con mutex interno). Antes TODOS los
// visitantes del mundo veían y movían la MISMA ventana — eso era inaceptable
// para un hosting público.
class WebServer {
public:
    struct Session {
        std::shared_ptr<app::BrowserShell> shell;
        std::atomic<unsigned long> frame_seq{0};
        std::chrono::steady_clock::time_point last_active{std::chrono::steady_clock::now()};
    };

    WebServer(int port, const std::string& host,
              std::shared_ptr<search::SearchIndex> shared_index = nullptr)
        : port_(port), host_(host),
          shared_index_(std::move(shared_index)) {
        if (!shared_index_) shared_index_ = std::make_shared<search::SearchIndex>();
    }

    search::SearchIndex& index() { return *shared_index_; }

    void run_synchronous();
    void start();
    void stop();

private:
    // Crea/recupera la sesión REAL de un visitante (con recolección LRU)
    std::shared_ptr<Session> get_session(const std::string& sid) {
        {
            std::lock_guard<std::mutex> lk(sessions_mx_);
            auto it = sessions_.find(sid);
            if (it != sessions_.end()) {
                it->second->last_active = std::chrono::steady_clock::now();
                return it->second;
            }
        }
        auto ses = std::make_shared<Session>();
        ses->shell = std::make_shared<app::BrowserShell>(shared_index_);
        ses->shell->set_data_path(pages_tsv_);
        {
            std::lock_guard<std::mutex> lk(sessions_mx_);
            // Cap duro honesto: 64 sesiones RAM ~ algún MB c/u.
            if (sessions_.size() >= MAX_SESSIONS) {
                auto oldest = sessions_.begin();
                for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
                    if (it->second->last_active < oldest->second->last_active) oldest = it;
                sessions_.erase(oldest);
            }
            sessions_[sid] = ses;
        }
        gc_sessions();
        return ses;
    }

    // Expira sesiones inactivas (>90 min) — memoria real bajo control
    void gc_sessions() {
        std::lock_guard<std::mutex> lk(sessions_mx_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            auto idle = std::chrono::duration_cast<std::chrono::minutes>(now - it->second->last_active).count();
            if (idle > 90) it = sessions_.erase(it); else ++it;
        }
    }

    static std::string new_session_id() {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        static const char* hex = "0123456789abcdef";
        std::string s(24, '0');
        for (auto& c : s) c = hex[rng() & 15];
        return s;
    }

    void handle_client(int client_sock);
    std::string handle_request(const std::string& method, const std::string& path,
                               const std::string& query, const std::string& body,
                               Session& ses, bool& is_binary);

    int port_;
    std::string host_;
    bool running_{false};
    std::unique_ptr<std::thread> server_thread_;

    // Índice COMPARTIDO entre sesiones + ruta de persistencia
    std::shared_ptr<search::SearchIndex> shared_index_;
    std::string pages_tsv_;

    std::mutex sessions_mx_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    static constexpr size_t MAX_SESSIONS = 64;

public:
    void set_data_path(const std::string& p) { pages_tsv_ = p; }
};

} // namespace nuby::server
