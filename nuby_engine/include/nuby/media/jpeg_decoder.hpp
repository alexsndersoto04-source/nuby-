#pragma once
// ============================================================================
// NUBY JPEG/GIF DECODER — Decodificador REAL vía ImageMagick delegate.
// Filosofía: NO simular. Nuby delega TLS a openssl s_client (real), así
// como Chrome delega a BoringSSL. Aquí delegamos JPEG/GIF a ImageMagick
// `convert` que a su vez usa libjpeg-turbo/libgif REALES del sistema.
// Es decodificación REAL, no placeholder. Si `convert` no está, se reporta
// error honesto (como con openssl sin TLS).
//
// Por qué no stb_image embebido: sandbox sin egress no permite descargar
// stb_image.h y no hay zlib headers para compilarlo. ImageMagick SÍ está
// disponible en el sandbox (convert 6.9.11 + libjpeg) y en Debian/Render
// se instala con `apt-get install imagemagick`. Es la vía más honesta y
// portátil ahora. Futuro: reemplazar por stb_image o libjpeg directo sin
// fork cuando haya headers disponibles — mismo resultado, más rápido.
//
// Formato intermedio: PPM P6 binario (P6\nW H\n255\n + RGB 3*W*H) — el más
// simple y sin compresión. Lo parseamos y convertimos a Image RGBA.
// ============================================================================

#include "image.hpp"
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <array>
#include <cctype>
#include <algorithm>

namespace nuby::media {

class JpegDecoder {
public:
    // Decodifica JPEG o GIF (detecta por magic o deja que convert auto-detecte).
    // `data` puede contener bytes nulos — std::string los preserva.
    // Retorna true si se decodificó y `out` queda válido (RGBA).
    static bool decode(const std::string& data, Image& out, std::string& err) {
        if (data.empty()) { err = "datos vacíos"; return false; }
        // Cap honesto: no decodificar imágenes gigantes que revienten RAM
        if (data.size() > 8 * 1024 * 1024) { err = "imagen demasiado grande (>8MB)"; return false; }

        // Intenta vía convert. Si falla, err queda con el motivo.
        std::string ppm;
        if (!run_convert(data, ppm, err)) return false;
        if (!parse_ppm(ppm, out, err)) return false;
        return true;
    }

private:
    static bool run_convert(const std::string& in_data, std::string& out_ppm, std::string& err) {
        int in_pipe[2], out_pipe[2];
        if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
            err = "no se pudieron crear pipes";
            return false;
        }
        pid_t pid = fork();
        if (pid < 0) {
            close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
            err = "fork() falló";
            return false;
        }
        if (pid == 0) {
            // HIJO
            dup2(in_pipe[0], STDIN_FILENO);
            dup2(out_pipe[1], STDOUT_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, STDERR_FILENO);
            close(in_pipe[0]); close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);
            // Intentamos `convert` (ImageMagick 6) y fallback a `magick` (IM7)
            // Usamos formato auto-detect: `convert - ppm:-` lee magic y escribe PPM.
            execlp("convert", "convert", "-", "ppm:-", (char*)nullptr);
            // Si execlp falla, probamos `magick`
            execlp("magick", "magick", "-", "ppm:-", (char*)nullptr);
            _exit(127);
        }
        // PADRE
        close(in_pipe[0]);
        close(out_pipe[1]);

        // Escribir JPEG/GIF completo al stdin de convert (binario seguro)
        size_t total = in_data.size();
        size_t written = 0;
        while (written < total) {
            ssize_t n = write(in_pipe[1], in_data.data() + written, total - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            written += (size_t)n;
        }
        close(in_pipe[1]); // EOF

        // Leer PPM de convert (binario seguro)
        out_ppm.clear();
        out_ppm.reserve(1024 * 1024);
        std::array<char, 16384> buf;
        for (;;) {
            ssize_t n = read(out_pipe[0], buf.data(), buf.size());
            if (n > 0) out_ppm.append(buf.data(), (size_t)n);
            else if (n == 0) break;
            else {
                if (errno == EINTR) continue;
                break;
            }
            if (out_ppm.size() > 16 * 1024 * 1024) { // cap 16MB PPM
                break;
            }
        }
        close(out_pipe[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            err = "ImageMagick 'convert' no está disponible en este sistema (instala imagemagick)";
            return false;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            if (out_ppm.empty()) {
                err = "convert falló (código " + std::to_string(WEXITSTATUS(status)) + ") — imagen corrupta o formato no soportado";
                return false;
            }
            // aún con código !=0, si hay PPM lo intentamos parsear
        }
        if (out_ppm.empty()) {
            err = "convert no produjo salida (imagen vacía o corrupta)";
            return false;
        }
        // Debe empezar con P6
        if (out_ppm.size() < 4 || out_ppm[0] != 'P' || out_ppm[1] != '6') {
            err = "salida de convert no es PPM P6";
            return false;
        }
        return true;
    }

    static bool parse_ppm(const std::string& ppm, Image& out, std::string& err) {
        size_t pos = 0;
        // Magic P6
        if (ppm.size() < 3 || ppm[0] != 'P' || ppm[1] != '6') { err = "PPM sin magic P6"; return false; }
        pos = 2;
        auto skip_ws_comments = [&]() {
            while (pos < ppm.size()) {
                if (ppm[pos] == '#') { // comentario
                    while (pos < ppm.size() && ppm[pos] != '\n') ++pos;
                } else if (std::isspace((unsigned char)ppm[pos])) {
                    ++pos;
                } else break;
            }
        };
        auto read_int = [&](int& v) -> bool {
            skip_ws_comments();
            if (pos >= ppm.size() || !std::isdigit((unsigned char)ppm[pos])) return false;
            v = 0;
            while (pos < ppm.size() && std::isdigit((unsigned char)ppm[pos])) {
                v = v * 10 + (ppm[pos] - '0');
                ++pos;
            }
            return true;
        };
        int w=0, h=0, maxval=0;
        if (!read_int(w) || !read_int(h) || !read_int(maxval)) { err = "cabecera PPM incompleta"; return false; }
        if (w <= 0 || h <= 0 || w > 8000 || h > 8000) { err = "dimensiones PPM inválidas " + std::to_string(w) + "x" + std::to_string(h); return false; }
        if (maxval != 255) { err = "PPM maxval !=255 no soportado"; return false; }
        // Un whitespace single tras maxval
        if (pos < ppm.size() && std::isspace((unsigned char)ppm[pos])) ++pos;
        else { err = "PPM sin whitespace tras maxval"; return false; }
        size_t header_len = pos;
        size_t need = (size_t)w * (size_t)h * 3;
        if (ppm.size() < header_len + need) { err = "PPM truncado: esperado " + std::to_string(need) + " bytes RGB, hay " + std::to_string(ppm.size() - header_len); return false; }
        // Convertir RGB -> RGBA (alpha 255)
        out.width = w;
        out.height = h;
        out.pixels.resize((size_t)w * (size_t)h);
        const unsigned char* src = (const unsigned char*)ppm.data() + header_len;
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            uint8_t r = src[i*3+0];
            uint8_t g = src[i*3+1];
            uint8_t b = src[i*3+2];
            out.pixels[i] = (0xFFu << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
        }
        return true;
    }
};

} // namespace nuby::media
