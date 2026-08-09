// ============================================================================
// NUBY WEB SERVER — Transporte de píxeles y eventos. NADA de UI aquí.
//
// Rutas:
//   GET  /              → el "monitor": un canvas vacío + JS que reenvía la
//                         entrada y dibuja los píxeles que mandó el motor.
//                         No contiene NI UN elemento de la UI de Nuby.
//   GET  /frame.seq     → {"seq":N} — versión del último frame del motor
//   GET  /frame.raw     → frame RGBA comprimido con RLE propio de Nuby
//   POST /event         → k=click|wheel|char|key  x,y / dy / cp / key
//   GET  /api/search?q= → búsqueda BM25 real (API auxiliar, datos reales)
//   GET  /api/stats     → métricas reales del índice
//
// Compresión: RLE estilo PackBits a nivel de píxel (formato propio,
// documentado en encode_rle). En páginas de texto comprime 10-40× de verdad.
// ============================================================================

#include "../../include/nuby/server/web_server.hpp"
#include "../../include/nuby/core/string_utils.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstring>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cctype>

namespace nuby::server {

// ---------------------------------------------------------------------------
// El MONITOR: no es la interfaz de Nuby. Es el "televisor".
// Cada píxel del canvas proviene del rasterizador del motor C++.
// ---------------------------------------------------------------------------
static std::string get_monitor_html() {
    return R"MONITOR(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Nuby — monitor remoto</title>
<style>
  html,body{margin:0;padding:0;background:#0b0b0c;height:100%;display:flex;
    flex-direction:column;align-items:center;justify-content:center;
    font-family:system-ui,sans-serif}
  #screen{image-rendering:pixelated;max-width:100vw;max-height:92vh;
    background:#fff;box-shadow:0 8px 40px rgba(0,0,0,.6);border-radius:6px}
  #hint{color:#666;font-size:12px;margin-top:10px;text-align:center}
  #kbd{position:fixed;left:-999px;top:0;opacity:.01}
</style>
</head>
<body>
<canvas id="screen" width="1024" height="640"></canvas>
<input id="kbd" autocomplete="off" autocapitalize="off">
<div id="hint">NUBY · Cada pixel de la imagen lo calculo el motor C++ —
este canvas solo los muestra (como un escritorio remoto). Click, rueda y
teclado viajan al motor. <span id="stat"></span></div>
<script>
(function(){
  var cv=document.getElementById('screen'), ctx=cv.getContext('2d');
  var kbd=document.getElementById('kbd'), stat=document.getElementById('stat');
  var W=1024,H=640,lastSeq=-1;
  var img=ctx.createImageData(W,H);

  function post(qs, then){
    var r=new XMLHttpRequest(); r.open('POST','/event?'+qs,true);
    r.send(null); if(then) then();
  }
  function scale(ev){
    var b=cv.getBoundingClientRect();
    return {x:Math.round((ev.clientX-b.left)*(W/b.width)),
            y:Math.round((ev.clientY-b.top)*(H/b.height))};
  }
  cv.addEventListener('mousedown',function(ev){
    var p=scale(ev);
    kbd.focus();
    post('k=click&x='+p.x+'&y='+p.y,function(){setTimeout(poll,120);setTimeout(poll,500);});
  });
  cv.addEventListener('wheel',function(ev){
    ev.preventDefault();
    post('k=wheel&dy='+Math.round(ev.deltaY),function(){setTimeout(poll,80);});
  },{passive:false});
  window.addEventListener('keydown',function(ev){
    if(ev.metaKey||ev.ctrlKey||ev.altKey)return;
    var k=ev.key;
    if(k==='Backspace'||k==='Enter'||k==='Escape'){
      ev.preventDefault();
      post('k=key&key='+k,function(){setTimeout(poll,150);setTimeout(poll,600);});
    } else if(k.length===1){
      ev.preventDefault();
      var cp=k.codePointAt(0);
      post('k=char&cp='+cp,function(){setTimeout(poll,120);});
    }
  });

  // Decodificador del RLE propio de Nuby (formato PackBits por píxel):
  // byte ctrl <128 → ctrl+1 píxeles literales; ≥129 → (257-ctrl) repeticiones.
  function decodeRLE(bytes, out){
    var src=12, px=0, dv=img.data;
    while(src<bytes.length && px<W*H){
      var c=bytes[src++];
      if(c<128){
        var n=c+1;
        for(var i=0;i<n;i++){
          dv[px*4]=bytes[src];dv[px*4+1]=bytes[src+1];
          dv[px*4+2]=bytes[src+2];dv[px*4+3]=255;src+=3;px++;
        }
      } else if(c>128){
        var rep=257-c;
        var r=bytes[src],g=bytes[src+1],b=bytes[src+2];src+=3;
        for(var j=0;j<rep;j++){
          dv[px*4]=r;dv[px*4+1]=g;dv[px*4+2]=b;dv[px*4+3]=255;px++;
        }
      } else src++;
    }
    ctx.putImageData(img,0,0);
  }

  var frames=0, t0=Date.now();
  function poll(){
    fetch('/frame.seq').then(function(r){return r.json()}).then(function(j){
      if(j.seq===lastSeq)return;
      fetch('/frame.raw').then(function(r){return r.arrayBuffer()}).then(function(ab){
        var bytes=new Uint8Array(ab);
        var dv=new DataView(ab);
        var w=dv.getUint32(0,true), h=dv.getUint32(4,true);
        if(w===W&&h===H){ decodeRLE(bytes,img); lastSeq=j.seq; frames++; }
        var secs=(Date.now()-t0)/1000;
        stat.textContent='· '+(ab.byteLength/1024).toFixed(0)+' KB/frame RLE · seq '+j.seq;
      });
    }).catch(function(){});
  }
  setInterval(poll,250);
  poll();
})();
</script>
</body>
</html>)MONITOR";
}

// ---------------------------------------------------------------------------
// RLE propio de Nuby (PackBits a nivel de píxel). Graba RGB (sin alfa).
// Header: [u32 w][u32 h][u32 len_datos]
// ---------------------------------------------------------------------------
static void put_u32(std::vector<unsigned char>& out, uint32_t v) {
    out.push_back((unsigned char)(v & 0xFF));
    out.push_back((unsigned char)((v >> 8) & 0xFF));
    out.push_back((unsigned char)((v >> 16) & 0xFF));
    out.push_back((unsigned char)((v >> 24) & 0xFF));
}

static std::vector<unsigned char> encode_rle(const std::vector<uint32_t>& px, int w, int h) {
    std::vector<unsigned char> out;
    put_u32(out, (uint32_t)w);
    put_u32(out, (uint32_t)h);
    put_u32(out, 0); // parche len luego

    size_t total = px.size();
    size_t i = 0;
    while (i < total) {
        // ¿cuántos píxeles iguales siguen?
        size_t run = 1;
        while (i + run < total && px[i + run] == px[i] && run < 128) ++run;
        if (run >= 3) {
            out.push_back((unsigned char)(257 - run)); // 129..254 → 3..128 reps
            out.push_back((unsigned char)(px[i] & 0xFF));          // B? no: guardamos R,G,B
            // nuestro framebuffer es 0xAARRGGBB → mandamos R,G,B
            // (corrijo orden: extraemos cada canal explícitamente)
            out.resize(out.size() - 1);
            out.push_back((unsigned char)((px[i] >> 16) & 0xFF)); // R
            out.push_back((unsigned char)((px[i] >> 8) & 0xFF));  // G
            out.push_back((unsigned char)(px[i] & 0xFF));         // B
            i += run;
            continue;
        }
        // literal: agrupa hasta 128 píxeles no-repetidos
        size_t lit_start = i;
        size_t lit_count = 0;
        while (i < total && lit_count < 128) {
            size_t r2 = 1;
            while (i + r2 < total && px[i + r2] == px[i] && r2 < 128) ++r2;
            if (r2 >= 3) break;
            i += r2;
            lit_count += r2;
        }
        out.push_back((unsigned char)(lit_count - 1));
        for (size_t k = 0; k < lit_count; ++k) {
            uint32_t p = px[lit_start + k];
            out.push_back((unsigned char)((p >> 16) & 0xFF));
            out.push_back((unsigned char)((p >> 8) & 0xFF));
            out.push_back((unsigned char)(p & 0xFF));
        }
    }

    uint32_t len = (uint32_t)(out.size() - 12);
    out[8]  = (unsigned char)(len & 0xFF);
    out[9]  = (unsigned char)((len >> 8) & 0xFF);
    out[10] = (unsigned char)((len >> 16) & 0xFF);
    out[11] = (unsigned char)((len >> 24) & 0xFF);
    return out;
}

// --- HTTP utilidades --------------------------------------------------------
static std::string http_response(int code, const std::string& reason,
                                 const std::string& ctype, const std::string& body) {
    std::ostringstream r;
    r << "HTTP/1.1 " << code << " " << reason << "\r\n"
      << "Content-Type: " << ctype << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Connection: close\r\n\r\n";
    r << body;
    return r.str();
}
static std::string http_response_bin(int code, const std::string& reason,
                                     const std::string& ctype, const std::vector<unsigned char>& body) {
    std::ostringstream r;
    r << "HTTP/1.1 " << code << " " << reason << "\r\n"
      << "Content-Type: " << ctype << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Connection: close\r\n\r\n";
    std::string head = r.str();
    std::string full(head.begin(), head.end());
    full.append((const char*)body.data(), body.size());
    return full;
}

static std::string qs_get(const std::string& query, const std::string& key) {
    size_t p = query.find(key + "=");
    if (p == std::string::npos) return "";
    p += key.size() + 1;
    size_t e = query.find('&', p);
    std::string v = (e == std::string::npos) ? query.substr(p) : query.substr(p, e - p);
    // decodifica %XX y '+'
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%' && i + 2 < v.size()) {
            try { out += (char)std::stoi(v.substr(i + 1, 2), nullptr, 16); i += 2; continue; } catch (...) {}
        }
        out += (v[i] == '+') ? ' ' : v[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
std::string WebServer::handle_request(const std::string& method, const std::string& path,
                                      const std::string& query, const std::string& body,
                                      Session& ses, bool& is_binary) {
    is_binary = false;
    (void)body;

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        return http_response(200, "OK", "text/html; charset=utf-8", get_monitor_html());
    }

    if (method == "GET" && path == "/frame.seq") {
        if (ses.shell->tick_blink()) ses.frame_seq++;
        if (ses.shell->dirty()) { ses.frame_seq++; ses.shell->clear_dirty(); }
        std::string j = "{\"seq\":" + std::to_string(ses.frame_seq.load()) + "}";
        return http_response(200, "OK", "application/json", j);
    }

    if (method == "GET" && path == "/frame.raw") {
        auto& fb = ses.shell->frame();
        if (fb.empty()) {
            return http_response(200, "OK", "application/octet-stream", "");
        }
        auto rle = encode_rle(fb, app::BrowserShell::W, app::BrowserShell::H);
        is_binary = true;
        return http_response_bin(200, "OK", "application/x-nuby-rle", rle);
    }

    if (method == "POST" && path == "/event") {
        std::string k = qs_get(query, "k");
        bool changed = false;
        if (k == "click") {
            int x = std::atoi(qs_get(query, "x").c_str());
            int y = std::atoi(qs_get(query, "y").c_str());
            changed = ses.shell->handle_click(x, y);
        } else if (k == "wheel") {
            int dy = std::atoi(qs_get(query, "dy").c_str());
            changed = ses.shell->handle_wheel(dy);
        } else if (k == "char") {
            unsigned long cp = std::strtoul(qs_get(query, "cp").c_str(), nullptr, 10);
            changed = ses.shell->handle_char((uint32_t)cp);
        } else if (k == "key") {
            changed = ses.shell->handle_key(qs_get(query, "key"));
        }
        if (changed) ses.frame_seq++;
        return http_response(200, "OK", "application/json",
            std::string("{\"ok\":") + (changed ? "true" : "false") + ",\"seq\":" +
            std::to_string(ses.frame_seq.load()) + "}");
    }

    if (method == "GET" && path == "/api/search") {
        std::string q = qs_get(query, "q");
        auto t0 = std::chrono::steady_clock::now();
        auto hits = ses.shell->search(q);
        long us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        std::ostringstream j;
        j << "{\"query\":\"" << q << "\",\"elapsed_us\":" << us << ",\"results\":[";
        for (size_t i = 0; i < hits.size(); ++i) {
            auto jsonesc = [](const std::string& s){
                std::string o; for (char c : s) {
                    if (c=='"') o += "\\\""; else if (c=='\\') o += "\\\\";
                    else if (c=='\n') o += " "; else o += c;
                } return o;
            };
            j << (i ? "," : "") << "{\"title\":\"" << jsonesc(hits[i].doc->title)
              << "\",\"url\":\"" << jsonesc(hits[i].doc->url)
              << "\",\"domain\":\"" << jsonesc(hits[i].doc->domain)
              << "\",\"score\":" << hits[i].score
              << ",\"snippet\":\"" << jsonesc(hits[i].snippet) << "\"}";
        }
        j << "]}";
        return http_response(200, "OK", "application/json; charset=utf-8", j.str());
    }

    // Navegación REAL (misma vía que teclear la URL en la barra):
    // GET /api/goto?u=<url> → el engine descarga y renderiza de verdad
    if (method == "GET" && path == "/api/goto") {
        std::string u = qs_get(query, "u");
        if (u.empty()) {
            return http_response(400, "Bad Request", "application/json",
                                 "{\"error\":\"falta parametro u\"}");
        }
        auto t0 = std::chrono::steady_clock::now();
        ses.shell->go(u);
        long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        ses.frame_seq++;
        auto jsonesc = [](const std::string& s){
            std::string o; for (char c : s) {
                if (c=='\"') o += "\\\""; else if (c=='\\') o += "\\\\";
                else if (c=='\n') o += " "; else o += c;
            } return o;
        };
        std::ostringstream j;
        j << "{\"ok\":true,\"elapsed_ms\":" << ms
          << ",\"url\":\"" << jsonesc(ses.shell->current_url())
          << "\",\"status\":\"" << jsonesc(ses.shell->status()) << "\"}";
        return http_response(200, "OK", "application/json; charset=utf-8", j.str());
    }

    if (method == "GET" && path == "/api/stats") {
        std::ostringstream j;
        j << "{\"documents\":" << shared_index_->document_count()
          << ",\"terms\":" << shared_index_->term_count()
          << ",\"history_entries\":" << ses.shell->history().size()
          << ",\"active_sessions\":" << ([this]{ std::lock_guard<std::mutex> lk(sessions_mx_); return sessions_.size(); })()
          << "}";
        return http_response(200, "OK", "application/json", j.str());
    }

    return http_response(404, "Not Found", "text/plain; charset=utf-8",
                         "404 — ruta desconocida\n");
}

void WebServer::handle_client(int client_sock) {
    char buffer[32768];
    ssize_t bytes_read = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) { close(client_sock); return; }
    buffer[bytes_read] = '\0';
    std::string request(buffer, bytes_read);

    std::istringstream req_stream(request);
    std::string method, path, proto;
    req_stream >> method >> path >> proto;

    std::string clean_path = path, query;
    size_t q = path.find('?');
    if (q != std::string::npos) { clean_path = path.substr(0, q); query = path.substr(q + 1); }

    // body (POST) si vino en el mismo paquete
    std::string body;
    size_t hdr_end = request.find("\r\n\r\n");
    if (hdr_end != std::string::npos) body = request.substr(hdr_end + 4);

    // ---- Sesión REAL por visitante (cookie nuby_sid) -----------------------
    std::string sid;
    {
        std::istringstream hs(request);
        std::string line;
        std::getline(hs, line); // request line
        while (std::getline(hs, line)) {
            if (line.empty() || line == "\r") break;
            if (line.rfind("Cookie:", 0) == 0) {
                std::string ch = line.substr(7);
                auto p = ch.find("nuby_sid=");
                if (p != std::string::npos) {
                    p += 9;
                    auto e = ch.find(';', p);
                    sid = ch.substr(p, e == std::string::npos ? std::string::npos : e - p);
                    sid.erase(std::remove_if(sid.begin(), sid.end(),
                        [](unsigned char c){ return c == '\r' || c == '\n' || c == ' '; }), sid.end());
                    bool ok = sid.size() == 24 &&
                        std::all_of(sid.begin(), sid.end(), [](unsigned char c){ return std::isxdigit(c); });
                    if (!ok) sid.clear();
                }
            }
        }
    }
    bool brand_new = sid.empty();
    if (brand_new) sid = new_session_id();
    auto ses = get_session(sid);

    bool is_binary = false;
    std::string resp = handle_request(method, clean_path, query, body, *ses, is_binary);

    if (brand_new) {
        // set-cookie una sola vez (hereda la sesión en los próximos requests)
        auto pos = resp.find("\r\n");
        if (pos != std::string::npos)
            resp.insert(pos, "\r\nSet-Cookie: nuby_sid=" + sid + "; Path=/; HttpOnly; SameSite=Lax");
    }
    send(client_sock, resp.data(), resp.size(), 0);
    close(client_sock);
}

void WebServer::run_synchronous() {
    int server_fd = -1;
    int current_port = port_;
    for (int attempt = 0; attempt < 15; ++attempt) {
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) { std::cerr << "socket() fallo\n"; return; }
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(current_port);
        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) >= 0) {
            port_ = current_port;
            break;
        }
        close(server_fd); server_fd = -1; current_port++;
    }
    if (server_fd < 0 || listen(server_fd, 64) < 0) {
        std::cerr << "No se pudo abrir el puerto.\n";
        return;
    }
    std::cout << "\033[1;32m[✔] Nuby sirviendo PIXELES DEL MOTOR en:\033[0m\n"
              << "    \033[1;34mhttp://localhost:" << port_ << "\033[0m  (el cliente es solo un monitor)\n";
    running_ = true;
    while (running_) {
        struct sockaddr_in client_addr{};
        socklen_t addrlen = sizeof(client_addr);
        int cs = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (cs >= 0) {
            // hilo por cliente + timeout de lectura REAL: antes todo era
            // secuencial y UN cliente lento congelaba a todos los demás
            struct timeval tv{20, 0};
            setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            std::thread(&WebServer::handle_client, this, cs).detach();
        }
    }
    close(server_fd);
}

void WebServer::start() {
    server_thread_ = std::make_unique<std::thread>(&WebServer::run_synchronous, this);
}
void WebServer::stop() {
    running_ = false;
    if (server_thread_ && server_thread_->joinable()) server_thread_->detach();
}

} // namespace nuby::server
