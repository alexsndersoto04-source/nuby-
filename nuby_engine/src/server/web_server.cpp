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
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover">
<title>Nuby</title>
<style>
  /* Pantalla completa, cero decoración técnica: esto es un navegador,
     no un póster de sí mismo. El canvas ES la ventana. */
  html,body{margin:0;padding:0;background:#fff;height:100%;width:100%;
    overflow:hidden;font-family:system-ui,sans-serif}
  /* touch-action:none + user-scalable=no: SIN esto Android interpretaba el
     toque como gesto de zoom/scroll, se llevaba el foco del input y el
     teclado virtual NUNCA abría (el bug que reportó el usuario 2026-08-09) */
  #screen{display:block;position:fixed;inset:0;width:100vw;height:100vh;
    image-rendering:auto;background:#fff;touch-action:none}
  /* El captador de teclado: INVISIBLE PERO DENTRO de la pantalla. Antes
     estaba a left:-999px y Chrome/Android se negaba a abrir el teclado
     virtual para un input fuera de la vista (bug real del móvil). */
  #kbd{position:fixed;left:8px;bottom:8px;width:44px;height:44px;
    opacity:.015;border:0;padding:0;z-index:10;pointer-events:none}
</style>
</head>
<body>
<canvas id="screen" width="1024" height="640"></canvas>
<input id="kbd" autocomplete="off" autocapitalize="off" autocorrect="off" spellcheck="false" inputmode="text" enterkeyhint="search" tabindex="0" aria-label="entrada de texto del navegador Nuby">
<script>
(function(){
  var cv=document.getElementById('screen'), ctx=cv.getContext('2d');
  var kbd=document.getElementById('kbd');
  var W=1024,H=640,lastSeq=-1;
  var img=ctx.createImageData(W,H);

  // Viewport real por sesión: le decimos al motor el tamaño EXACTO de
  // nuestra ventana (el canvas la cubre entera) y él renderiza a medida.
  var lastSentW=0, lastSentH=0, resizeTimer=null;
  function sendSize(){
    var w=Math.max(240,Math.min(1920,window.innerWidth));
    var h=Math.max(320,Math.min(2160,window.innerHeight));
    if(w===lastSentW&&h===lastSentH)return;
    lastSentW=w;lastSentH=h;
    post('k=resize&w='+w+'&h='+h,function(){setTimeout(poll,200);setTimeout(poll,700);});
  }
  window.addEventListener('resize',function(){
    if(resizeTimer)clearTimeout(resizeTimer);
    resizeTimer=setTimeout(sendSize,350);
  });

  // ---- Puente de teclado REAL --------------------------------------------
  // kbText = el texto que el MOTOR tiene ahora mismo en el campo activo.
  // El servidor lo reporta en cada respuesta de /event (kbd/text). Así el
  // autocorrector, la predicción y el borrado trabajan sobre el texto real.
  var kbText='';
  function syncKb(j){
    if(!j||typeof j.kbd==='undefined')return;
    if(j.kbd){
      if(typeof j.text==='string'){ kbText=j.text; if(kbd.value!==j.text) kbd.value=j.text; }
    } else {
      kbText=''; if(kbd.value) kbd.value='';
      if(document.activeElement===kbd) kbd.blur(); // el motor salió del campo → teclado se cierra SOLO
    }
  }
  function post(qs, then){
    var r=new XMLHttpRequest(); r.open('POST','/event?'+qs,true);
    r.onreadystatechange=function(){
      if(r.readyState===4&&r.status===200){ try{syncKb(JSON.parse(r.responseText));}catch(e){} }
    };
    r.send(null); if(then) then();
  }
  function scale(ev){
    var b=cv.getBoundingClientRect();
    return {x:Math.round((ev.clientX-b.left)*(W/b.width)),
            y:Math.round((ev.clientY-b.top)*(H/b.height))};
  }
  function tap(p){
    // Foco OPTIMISTA dentro del gesto: Android solo abre el teclado virtual
    // si focus() ocurre procesando un toque real. Si el toque NO cae en un
    // campo de texto, la respuesta {kbd:false} llamará a blur() y se cierra.
    kbd.focus();
    post('k=click&x='+p.x+'&y='+p.y,function(){setTimeout(poll,120);setTimeout(poll,500);});
  }
  cv.addEventListener('mousedown',function(ev){ tap(scale(ev)); });
  cv.addEventListener('wheel',function(ev){
    ev.preventDefault();
    post('k=wheel&dy='+Math.round(ev.deltaY),function(){setTimeout(poll,80);});
  },{passive:false});
  // Scroll táctil: arrastrar el dedo = rueda (traducción honesta, la
  // pantalla remota no tiene eventos táctiles nativos)
  var lastTouchY=null, touchMoved=false, touchSX=0, touchSY=0;
  cv.addEventListener('touchstart',function(ev){
    var t=ev.touches[0]; if(!t)return;
    touchMoved=false; touchSX=t.clientX; touchSY=t.clientY;
    lastTouchY = ev.touches.length===1 ? t.clientY : null;
  },{passive:true});
  document.addEventListener('touchmove',function(ev){
    if(ev.touches.length!==1)return;
    ev.preventDefault();
    var t=ev.touches[0];
    if(Math.abs(t.clientX-touchSX)>10||Math.abs(t.clientY-touchSY)>10) touchMoved=true;
    var y=t.clientY;
    if(lastTouchY!==null){
      var dy=lastTouchY-y;
      if(Math.abs(dy)>10){post('k=wheel&dy='+Math.round(dy*2.2),function(){setTimeout(poll,90);});lastTouchY=y;}
    }
  },{passive:false});
  cv.addEventListener('touchend',function(ev){
    lastTouchY=null;
    if(touchMoved) return; // fue arrastre de scroll, no un toque
    // Toque REAL: cancelamos el mouse sintético (si no, el motor recibiría
    // el click DOS veces: touchend y luego el mousedown fantasma de Chrome).
    ev.preventDefault();
    var t=ev.changedTouches[0];
    tap(scale(t));
  });

  // TEXTO: camino principal = evento 'input' del captador (sobrevive a
  // teclados virtuales, IME, autocorrector y pegar; keydown NO llega
  // confiable desde el teclado del teléfono — bug real del móvil).
  // Hay DIFF de verdad contra kbText (lo que tiene el motor): se mandan
  // Backspaces por lo borrado y chars por lo añadido. El autocorrector ya
  // no infla la búsqueda porque solo viaja la DIFERENCIA, no todo el valor.
  kbd.addEventListener('input',function(){
    var v=kbd.value;
    var p=0, max=Math.min(v.length,kbText.length);
    while(p<max&&v.charCodeAt(p)===kbText.charCodeAt(p))p++;
    for(var d=0,ds=kbText.length-p;d<ds;d++)
      post('k=key&key=Backspace',function(){setTimeout(poll,120);});
    for(var i=p;i<v.length;i++){
      var cp=v.codePointAt(i); if(cp>0xFFFF)i++; // pares suplentes UTF-16
      (function(c){post('k=char&cp='+c,function(){setTimeout(poll,110);});})(cp);
    }
    kbText=v;
  });
  window.addEventListener('keydown',function(ev){
    if(ev.metaKey||ev.ctrlKey||ev.altKey)return;
    var k=ev.key;
    if(k==='Backspace'&&ev.target===kbd)return; // el 'input' de arriba ya lo manda (evita doble borrado)
    if(k==='Backspace'||k==='Enter'||k==='Escape'){
      ev.preventDefault();
      post('k=key&key='+k,function(){setTimeout(poll,150);setTimeout(poll,600);});
    } else if(k.length===1 && ev.target!==kbd){
      // PC con foco fuera del captador: mandar la letra directo
      ev.preventDefault();
      var cp=k.codePointAt(0);
      post('k=char&cp='+cp,function(){setTimeout(poll,120);});
    }
    // si el foco está EN kbd, el evento 'input' de arriba hace el trabajo
    // (evita enviar cada letra DOS veces — bug que inflaba las búsquedas)
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

  function poll(){
    fetch('/frame.seq').then(function(r){return r.json()}).then(function(j){
      if(j.seq===lastSeq)return;
      fetch('/frame.raw').then(function(r){return r.arrayBuffer()}).then(function(ab){
        var bytes=new Uint8Array(ab);
        var dv=new DataView(ab);
        var w=dv.getUint32(0,true), h=dv.getUint32(4,true);
        // si el motor cambió el tamaño (nuestro resize), re-crear el canvas
        if(w!==W||h!==H){
          W=w;H=h;cv.width=w;cv.height=h;img=ctx.createImageData(w,h);
        }
        decodeRLE(bytes,img); lastSeq=j.seq;
      });
    }).catch(function(){});
  }
  setInterval(poll,250);
  poll();
  sendSize(); // primera negociación de viewport al cargar
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

// Escape JSON compartido (antes había dos lambdas duplicadas)
static std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += " ";
        else o += c;
    }
    return o;
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
        // el tamaño es el de ESTA sesión (viewport dinámico real)
        auto rle = encode_rle(fb, ses.shell->width(), ses.shell->height());
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
        } else if (k == "resize") {
            // el monitor reporta el tamaño real de su pantalla
            int w = std::atoi(qs_get(query, "w").c_str());
            int h = std::atoi(qs_get(query, "h").c_str());
            if (w > 0 && h > 0) {
                int old_w = ses.shell->width(), old_h = ses.shell->height();
                ses.shell->set_viewport(w, h);
                changed = (ses.shell->width() != old_w || ses.shell->height() != old_h);
            }
        } else if (k == "wheel") {
            int dy = std::atoi(qs_get(query, "dy").c_str());
            changed = ses.shell->handle_wheel(dy);
        } else if (k == "char") {
            unsigned long cp = std::strtoul(qs_get(query, "cp").c_str(), nullptr, 10);
            if (cp > 0) changed = ses.shell->handle_char((uint32_t)cp); // cp=0 = basura
        } else if (k == "key") {
            changed = ses.shell->handle_key(qs_get(query, "key"));
        }
        if (changed) ses.frame_seq++;
        // Puente de teclado REAL para el monitor: le decimos si tras este
        // evento hay un campo de texto activo (kbd) y su contenido actual
        // (text). Con eso el teléfono abre/cierra el teclado virtual EN EL
        // MOMENTO correcto y el autocorrector trabaja sobre el texto real.
        std::ostringstream ev;
        ev << "{\"ok\":" << (changed ? "true" : "false")
           << ",\"seq\":" << ses.frame_seq.load()
           << ",\"kbd\":" << (ses.shell->text_focused() ? "true" : "false")
           << ",\"text\":\"" << json_escape(ses.shell->focused_text()) << "\"}";
        return http_response(200, "OK", "application/json", ev.str());
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
          << ",\"downloads\":" << ses.shell->downloads().size()
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
