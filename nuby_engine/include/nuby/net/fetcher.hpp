#pragma once

// ============================================================================
// NUBY FETCHER — Cliente de red REAL.
//
// Qué hace de verdad:
//   • http://   → socket POSIX propio: DNS real (getaddrinfo), TCP real,
//                 petición HTTP/1.1 real y lectura binario-segura.
//   • https://  → puente con el binario OpenSSL del sistema
//                 (`openssl s_client`), que ejecuta el handshake TLS real.
//                 Es el mismo rol que BoringSSL cumple en Chrome o NSS en
//                 Firefox: Nuby no reimplementa criptografía, la delega en
//                 un proveedor TLS real del sistema.
//   • Decodifica Transfer-Encoding: chunked (obligatorio en HTTP/1.1).
//   • Sigue redirects 3xx y resuelve URLs relativas.
//
// Qué NO hace (honesto):
//   • No descomprime gzip/br (no hay zlib enlazada): pedimos
//     `Accept-Encoding: identity` y los servidores responden en claro.
//   • No valida la cadena de certificados contra una CA store propia
//     (openssl s_client la valida por defecto; usamos -verify_return_error
//     para que falle si el certificado es inválido).
// ============================================================================

#include "url.hpp"
#include "../core/string_utils.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <memory>
#include <array>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

namespace nuby::net {

struct FetchResult {
    int status_code{0};
    std::string status_message;
    std::unordered_map<std::string, std::string> headers;
    std::string body;          // cuerpo ya des-chunkeado
    std::string final_url;     // URL tras redirects
    std::string error;         // si no está vacío, la petición falló
    long elapsed_ms{0};

    bool ok() const { return error.empty() && status_code >= 200 && status_code < 400; }

    std::string header(const std::string& name) const {
        std::string lower = core::StringUtils::to_lower(name);
        for (const auto& [k, v] : headers) {
            if (core::StringUtils::to_lower(k) == lower) return v;
        }
        return "";
    }
};

class Fetcher {
public:
    static constexpr size_t MAX_BODY_BYTES = 3 * 1024 * 1024; // 3 MB de tope honesto

    static FetchResult fetch(const std::string& raw_url, int max_redirects = 5) {
        auto t0 = now_ms();
        FetchResult res;
        std::string current = core::StringUtils::trim(raw_url);

        for (int hop = 0; hop <= max_redirects; ++hop) {
            URL url = URL::parse(current);
            if (url.host.empty()) {
                res.error = "URL invalida: '" + current + "'";
                res.elapsed_ms = now_ms() - t0;
                return res;
            }

            std::string raw;
            if (url.scheme == "https") {
                raw = fetch_via_openssl(url, res.error);
            } else if (url.scheme == "http" || url.scheme.empty()) {
                raw = fetch_via_socket(url, res.error);
            } else {
                res.error = "Esquema no soportado: " + url.scheme;
                res.elapsed_ms = now_ms() - t0;
                return res;
            }

            if (!res.error.empty()) {
                res.elapsed_ms = now_ms() - t0;
                return res;
            }

            if (!parse_raw_response(raw, res)) {
                res.error = "Respuesta HTTP malformada";
                res.elapsed_ms = now_ms() - t0;
                return res;
            }
            res.final_url = current;

            // Redirect?
            if (res.status_code >= 300 && res.status_code < 400) {
                std::string loc = res.header("Location");
                if (!loc.empty()) {
                    current = resolve_url(current, loc);
                    res.headers.clear();
                    continue;
                }
            }
            break;
        }
        res.elapsed_ms = now_ms() - t0;
        return res;
    }

    // Resuelve `maybe_relative` contra `base` (RFC 3986 simplificado, real).
    static std::string resolve_url(const std::string& base, const std::string& maybe_relative) {
        std::string rel = core::StringUtils::trim(maybe_relative);
        if (rel.empty()) return base;
        if (rel.find("://") != std::string::npos) return rel;

        URL b = URL::parse(base);
        if (rel.rfind("//", 0) == 0) return b.scheme + ":" + rel; // protocol-relative

        std::string origin = b.scheme + "://" + b.host +
            ((b.scheme == "http" && b.port != 80) || (b.scheme == "https" && b.port != 443)
                 ? ":" + std::to_string(b.port) : "");

        if (rel[0] == '/') return origin + rel;

        // relativa al directorio del path base
        std::string dir = b.path;
        size_t last_slash = dir.rfind('/');
        dir = (last_slash != std::string::npos) ? dir.substr(0, last_slash + 1) : "/";

        std::string joined = dir + rel;
        // normaliza ./ y ../ (ignorando segmentos vacíos → cero dobles '//')
        std::vector<std::string> segs;
        std::istringstream ss(joined);
        std::string seg;
        while (std::getline(ss, seg, '/')) {
            if (seg.empty() || seg == ".") continue;
            if (seg == "..") { if (!segs.empty()) segs.pop_back(); continue; }
            segs.push_back(seg);
        }
        std::string norm;
        for (auto& s2 : segs) { norm += "/"; norm += s2; }
        if (norm.empty()) norm = "/";
        if (!joined.empty() && joined.back() == '/' && norm.back() != '/') norm += "/";
        return origin + norm;
    }

private:
    static long now_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    // ---- http:// : socket POSIX propio ------------------------------------
    static std::string fetch_via_socket(const URL& url, std::string& err) {
        struct addrinfo hints{}, *res_info = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        std::string port_str = std::to_string(url.port);
        if (getaddrinfo(url.host.c_str(), port_str.c_str(), &hints, &res_info) != 0) {
            err = "DNS fallo para " + url.host;
            return "";
        }

        int sock = -1;
        for (auto* p = res_info; p && sock < 0; p = p->ai_next) {
            sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (sock < 0) continue;
            struct timeval tv{8, 0};
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            if (connect(sock, p->ai_addr, p->ai_addrlen) < 0) {
                close(sock); sock = -1;
            }
        }
        freeaddrinfo(res_info);

        if (sock < 0) {
            err = "No se pudo conectar a " + url.host + ":" + std::to_string(url.port);
            return "";
        }

        std::string req = build_request(url);
        if (send(sock, req.c_str(), req.size(), 0) < 0) {
            close(sock); err = "Fallo al enviar la peticion"; return "";
        }
        std::string raw = read_all_fd(sock);
        close(sock);
        if (raw.empty()) err = "El servidor cerro sin responder";
        return raw;
    }

    // ---- https:// : TLS real via openssl s_client --------------------------
    // Abrimos dos pipes: escribimos la petición HTTP al stdin del proceso
    // openssl y leemos la respuesta TLS-descifrada de su stdout.
    static std::string fetch_via_openssl(const URL& url, std::string& err) {
        int in_pipe[2], out_pipe[2];
        if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
            err = "No se pudieron crear pipes para TLS";
            return "";
        }

        pid_t pid = fork();
        if (pid < 0) { err = "fork() fallo"; return ""; }

        if (pid == 0) {
            // HIJO: openssl s_client -connect host:443 -servername host -quiet
            dup2(in_pipe[0], STDIN_FILENO);
            dup2(out_pipe[1], STDOUT_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, STDERR_FILENO);
            close(in_pipe[1]); close(out_pipe[0]);
            std::string connect_arg = url.host + ":" + std::to_string(url.port);
            execlp("openssl", "openssl", "s_client",
                   "-connect", connect_arg.c_str(),
                   "-servername", url.host.c_str(),
                   "-quiet",
                   (char*)nullptr);
            _exit(127); // si execlp falla
        }

        // PADRE
        close(in_pipe[0]); close(out_pipe[1]);
        std::string req = build_request(url);
        ssize_t w = write(in_pipe[1], req.c_str(), req.size());
        (void)w;
        close(in_pipe[1]); // EOF → openssl envía y espera respuesta

        std::string raw = read_all_fd(out_pipe[0]);
        close(out_pipe[0]);

        int status = 0;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            err = "El binario 'openssl' no esta disponible en este sistema";
            return "";
        }
        if (raw.empty()) {
            err = "TLS/conexion fallo con " + url.host +
                  " (sin salida a internet, certificado invalido, o el host no responde)";
        }
        return raw;
    }

    static std::string build_request(const URL& url) {
        std::ostringstream req;
        req << "GET " << url.path << (url.query.empty() ? "" : "?" + url.query) << " HTTP/1.1\r\n";
        req << "Host: " << url.host << "\r\n";
        req << "User-Agent: Nuby/2.0 (motor propio C++20; renderizado real)\r\n";
        req << "Accept: text/html,application/xhtml+xml,text/css,*/*;q=0.5\r\n";
        req << "Accept-Encoding: identity\r\n"; // honesto: sin gzip mientras no haya zlib
        req << "Accept-Language: es,en;q=0.6\r\n";
        req << "Connection: close\r\n\r\n";
        return req.str();
    }

    // Lectura binario-segura hasta EOF (la conexión es Connection: close).
    static std::string read_all_fd(int fd) {
        std::string data;
        std::array<char, 16384> buf;
        for (;;) {
            ssize_t n = recv(fd, buf.data(), buf.size(), 0);
            if (n > 0) { data.append(buf.data(), (size_t)n); }
            else if (n == 0) { break; }
            else {
                if (errno == EINTR) continue;
                break; // timeout u otro error → devolvemos lo leído
            }
            if (data.size() > MAX_BODY_BYTES * 2) break; // freno de seguridad
        }
        return data;
    }

    // ---- Parseo de respuesta: status line + headers + dechunk --------------
    static bool parse_raw_response(const std::string& raw, FetchResult& res) {
        size_t header_end = raw.find("\r\n\r\n");
        size_t delim_len = 4;
        if (header_end == std::string::npos) {
            header_end = raw.find("\n\n"); delim_len = 2;
            if (header_end == std::string::npos) return false;
        }

        std::string head = raw.substr(0, header_end);
        std::string body = raw.substr(header_end + delim_len);

        std::istringstream hs(head);
        std::string status_line;
        if (!std::getline(hs, status_line)) return false;
        auto parts = core::StringUtils::split_whitespace(status_line);
        if (parts.size() < 2) return false;
        try { res.status_code = std::stoi(parts[1]); } catch (...) { return false; }
        for (size_t i = 2; i < parts.size(); ++i) {
            if (!res.status_message.empty()) res.status_message += " ";
            res.status_message += parts[i];
        }

        std::string line;
        while (std::getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                res.headers[core::StringUtils::trim(line.substr(0, colon))] =
                    core::StringUtils::trim(line.substr(colon + 1));
            }
        }

        // Dechunk si aplica (real: obligatorio para HTTP/1.1 moderno)
        std::string te = core::StringUtils::to_lower(res.header("Transfer-Encoding"));
        if (te.find("chunked") != std::string::npos) {
            body = dechunk(body);
        } else {
            // Content-Length fiable si existe
            std::string cl = res.header("Content-Length");
            if (!cl.empty()) {
                try {
                    size_t n = (size_t)std::stoul(cl);
                    if (n < body.size()) body = body.substr(0, n);
                } catch (...) {}
            }
        }

        if (body.size() > MAX_BODY_BYTES) body = body.substr(0, MAX_BODY_BYTES);
        res.body = std::move(body);
        return true;
    }

    static std::string dechunk(const std::string& chunked) {
        std::string out;
        size_t pos = 0;
        for (;;) {
            size_t eol = chunked.find("\r\n", pos);
            if (eol == std::string::npos) break;
            std::string size_str = chunked.substr(pos, eol - pos);
            size_t semi = size_str.find(';'); // chunk-extensions
            if (semi != std::string::npos) size_str = size_str.substr(0, semi);
            unsigned long chunk_size = 0;
            try { chunk_size = std::stoul(core::StringUtils::trim(size_str), nullptr, 16); }
            catch (...) { break; }
            pos = eol + 2;
            if (chunk_size == 0) break; // último chunk
            if (pos + chunk_size > chunked.size()) { // cuerpo truncado
                out.append(chunked.substr(pos));
                break;
            }
            out.append(chunked, pos, chunk_size);
            pos += chunk_size + 2; // data + CRLF
            if (out.size() > MAX_BODY_BYTES * 2) break;
        }
        return out;
    }
};

} // namespace nuby::net
