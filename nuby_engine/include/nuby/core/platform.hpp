#pragma once

// ============================================================================
// CAPA DE PLATAFORMA DEL MOTOR NUBY
//
// El motor Nuby corre en dos plataformas:
//
//   1. Nativos (Linux/macOS/Windows): usa los recursos que da el sistema
//      operativo: hilos + std::mutex, ficheros con std::fstream.
//
//   2. WebAssembly (el motor compilado a wasm y ejecutado dentro del
//      navegador del visitante, sin servidor). El navegador NO da hilos ni
//      sockets ni un sistema de archivos real: da un sistema de archivos
//      virtual y un único hilo de ejecución. Por eso aquí:
//        • Mutex/LockGuard son reales pero sin bloqueo: en un solo hilo no
//          existe concurrencia que proteger (no se "simula" nada, se usa el
//          equivalent correcto para un solo hilo).
//        • Los ficheros se leen con las llamadas POSIX (open/read/close) que
//          WASI sí soporta, en vez de std::fstream, que libc++ no ofrece
//          para wasm32-wasi.
//
// Esta capa NO cambia la lógica del motor: parser, CSS, layout, rasterizado,
// fuentes y buscador son exactamente el mismo código en las dos plataformas.
// ============================================================================

#include <string>
#include <cstddef>

#if defined(__wasi__)

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>

namespace nuby::core {

// Un solo hilo => el candado no necesita bloquear. La API es la misma que la
// de std::mutex para que el código del motor no cambie.
class Mutex {
public:
    void lock() noexcept {}
    void unlock() noexcept {}
    bool try_lock() noexcept { return true; }
};

template <class M>
class LockGuard {
public:
    explicit LockGuard(M& m) : m_(m) { m_.lock(); }
    ~LockGuard() { m_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
private:
    M& m_;
};

inline bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

// Lee un fichero completo del sistema de archivos virtual de WASI.
inline std::string read_file_binary(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return std::string();
    std::string out;
    char buf[65536];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    ::close(fd);
    return out;
}

// La build de navegador es de SOLO LECTURA: una página web no puede escribir
// en el disco de quien la visita. El motor lo sabe y no lo intenta.
inline bool write_file_binary(const std::string&, const std::string&) { return false; }

} // namespace nuby::core

#else  // ------------------------- plataformas nativas -------------------------

#include <mutex>
#include <fstream>
#include <filesystem>
#include <iterator>

namespace nuby::core {

using Mutex = std::mutex;

template <class M>
class LockGuard {
public:
    explicit LockGuard(M& m) : m_(m) { m_.lock(); }
    ~LockGuard() { m_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
private:
    M& m_;
};

inline bool file_exists(const std::string& path) {
    return std::filesystem::exists(path);
}

inline std::string read_file_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

inline bool write_file_binary(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
}

} // namespace nuby::core

#endif
