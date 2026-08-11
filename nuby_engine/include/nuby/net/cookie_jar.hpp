#pragma once
// ============================================================================
// NUBY COOKIE JAR — Almacenamiento REAL de cookies HTTP (RFC 6265 simplificado).
//
// Por qué existe: sin cookies no hay login, sesión, carrito, nada. El fetcher
// antes era stateless: cada request era virgen. Ahora cada BrowserShell
// (cada visitante/sesión) tiene su propio jar aislado, como un navegador real.
//
// Qué hace de verdad:
//   • Parsea Set-Cookie real: name=value; Path=/; Domain=example.com; Max-Age=3600;
//     Expires=...; Secure; HttpOnly
//   • Domain matching real: hostOnly vs Domain (sufijo con dot)
//   • Path matching real: request path debe empezar por cookie path
//   • Genera header Cookie: a=b; c=d real para el host/path correctos
//   • Expiración básica via Max-Age (Expires se ignora por simplicidad honesta)
//   • Secure flag: solo envía si es https
//
// Qué NO hace aún (honesto):
//   • No maneja SameSite, Priority, Partitioned
//   • No persiste a disco (vive en RAM de la sesión, como en modo incógnito)
//   • No valida public suffix list
// ============================================================================

#include "../core/string_utils.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cctype>
#include <algorithm>
#include <sstream>

namespace nuby::net {

struct Cookie {
    std::string name;
    std::string value;
    std::string domain; // sin punto inicial, lower
    std::string path{"/"};
    bool hostOnly{true};
    bool secure{false};
    bool httpOnly{false};
    long long expires_at_ms{0}; // 0 = sesión
    bool is_expired() const {
        if (expires_at_ms == 0) return false;
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return now > expires_at_ms;
    }
};

class CookieJar {
public:
    // Añade cookies desde un header Set-Cookie crudo, asociado al host que lo envió.
    // request_host y request_path vienen de la URL que produjo el Set-Cookie.
    void add_from_header(const std::string& set_cookie_header,
                         const std::string& request_host,
                         const std::string& request_path = "/") {
        std::string s = core::StringUtils::trim(set_cookie_header);
        if (s.empty()) return;
        // Partir por ';'
        std::vector<std::string> parts;
        std::string cur;
        for (char c : s) {
            if (c == ';') { parts.push_back(cur); cur.clear(); }
            else cur += c;
        }
        parts.push_back(cur);
        if (parts.empty()) return;

        // Primera parte: name=value
        std::string nv = core::StringUtils::trim(parts[0]);
        size_t eq = nv.find('=');
        if (eq == std::string::npos) return;
        std::string name = core::StringUtils::trim(nv.substr(0, eq));
        std::string value = core::StringUtils::trim(nv.substr(eq+1));
        if (name.empty()) return;

        Cookie ck;
        ck.name = name;
        ck.value = value;
        ck.domain = core::StringUtils::to_lower(request_host);
        ck.hostOnly = true;
        // Path default: directorio de request_path
        ck.path = default_path(request_path);

        // Atributos
        for (size_t i = 1; i < parts.size(); ++i) {
            std::string attr = core::StringUtils::trim(parts[i]);
            size_t eq2 = attr.find('=');
            std::string k = core::StringUtils::to_lower(core::StringUtils::trim(eq2==std::string::npos?attr:attr.substr(0,eq2)));
            std::string v = eq2==std::string::npos?"":core::StringUtils::trim(attr.substr(eq2+1));
            if (k == "domain") {
                std::string d = core::StringUtils::to_lower(core::StringUtils::trim(v));
                if (!d.empty()) {
                    if (d[0]=='.') d = d.substr(1);
                    // Validación básica: el request_host debe terminar en domain o ser igual
                    if (domain_matches(request_host, d)) {
                        ck.domain = d;
                        ck.hostOnly = false;
                    } else {
                        // Domain inválido (no es sufijo) — se ignora la cookie (real, como navegadores)
                        return;
                    }
                }
            } else if (k == "path") {
                if (!v.empty() && v[0]=='/') ck.path = v;
            } else if (k == "max-age") {
                try {
                    long secs = std::stol(v);
                    if (secs <= 0) ck.expires_at_ms = 1; // expirada ya
                    else {
                        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        ck.expires_at_ms = now + secs*1000LL;
                    }
                } catch (...) {}
            } else if (k == "expires") {
                // Ignoramos Expires por simplicidad (Max-Age tiene prioridad). Honesto.
            } else if (k == "secure") {
                ck.secure = true;
            } else if (k == "httponly") {
                ck.httpOnly = true;
            }
        }

        // Expirada ya? borrar
        if (ck.is_expired()) {
            remove_cookie(ck.domain, ck.path, ck.name);
            return;
        }

        // Guardar: reemplazar si ya existe mismo name+domain+path
        std::string key = ck.domain + "|" + ck.path;
        auto& vec = jar_[key];
        for (auto& existing : vec) {
            if (existing.name == ck.name && existing.domain == ck.domain && existing.path == ck.path) {
                existing = ck;
                return;
            }
        }
        vec.push_back(std::move(ck));
    }

    // Genera el valor para el header `Cookie:` para un host/path dados.
    // secure_request = true si es https.
    std::string header_for(const std::string& host, const std::string& path, bool secure_request) const {
        std::string h = core::StringUtils::to_lower(host);
        std::string req_path = path.empty()?"/":path;
        std::vector<std::string> pairs;
        for (auto& kv : jar_) {
            for (auto& ck : kv.second) {
                if (ck.is_expired()) continue;
                if (ck.secure && !secure_request) continue;
                if (!domain_matches_cookie(h, ck)) continue;
                if (!path_matches(req_path, ck.path)) continue;
                pairs.push_back(ck.name + "=" + ck.value);
            }
        }
        if (pairs.empty()) return "";
        std::string out;
        for (size_t i=0;i<pairs.size();++i) {
            if (i) out += "; ";
            out += pairs[i];
        }
        return out;
    }

    // Para tests / debug
    size_t size() const {
        size_t n=0;
        for (auto& kv: jar_) n+=kv.second.size();
        return n;
    }
    void clear() { jar_.clear(); }

    // Para persistencia / inspección (honesto)
    std::vector<Cookie> all_cookies() const {
        std::vector<Cookie> out;
        for (auto& kv: jar_) for (auto& c: kv.second) if (!c.is_expired()) out.push_back(c);
        return out;
    }

private:
    // jar key = domain|path
    std::unordered_map<std::string, std::vector<Cookie>> jar_;

    static std::string default_path(const std::string& request_path) {
        if (request_path.empty() || request_path[0] != '/') return "/";
        size_t last_slash = request_path.rfind('/');
        if (last_slash == 0) return "/";
        return request_path.substr(0, last_slash);
    }
    static bool domain_matches(const std::string& request_host, const std::string& cookie_domain) {
        std::string h = core::StringUtils::to_lower(request_host);
        std::string d = core::StringUtils::to_lower(cookie_domain);
        if (h == d) return true;
        if (h.size() > d.size() && h.substr(h.size()-d.size()-1) == "."+d) return true;
        return false;
    }
    static bool domain_matches_cookie(const std::string& request_host, const Cookie& ck) {
        if (ck.hostOnly) return request_host == ck.domain;
        return domain_matches(request_host, ck.domain);
    }
    static bool path_matches(const std::string& request_path, const std::string& cookie_path) {
        if (request_path == cookie_path) return true;
        if (request_path.rfind(cookie_path, 0) == 0) {
            if (cookie_path.back() == '/') return true;
            if (request_path.size() > cookie_path.size() && request_path[cookie_path.size()]=='/') return true;
        }
        return false;
    }
    void remove_cookie(const std::string& domain, const std::string& path, const std::string& name) {
        std::string key = domain + "|" + path;
        auto it = jar_.find(key);
        if (it==jar_.end()) return;
        auto& vec = it->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const Cookie& c){ return c.name==name; }), vec.end());
        if (vec.empty()) jar_.erase(it);
    }
};

} // namespace nuby::net
