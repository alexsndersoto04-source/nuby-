// ============================================================================
// NUBY — RUNTIME WASI MÍNIMO PARA EL NAVEGADOR
//
// El motor Nuby se compila a wasm32-wasi. Este archivo implementa, en
// JavaScript, las llamadas que el motor espera del sistema (WASI preview 1):
// ficheros, reloj, azar y salida. No emula nada del motor: solo le da un
// "sistema operativo" mínimo, que es exactamente lo que un navegador puede dar.
//
// Ficheros: sistema de archivos VIRTUAL en memoria. El anfitrión decide qué
// hay dentro (el TSV real del índice y las fuentes TrueType del motor).
// ============================================================================

(function () {
  "use strict";

  // ---- errno de WASI (solo los que el motor puede encontrarse) -------------
  const ERRNO = {
    SUCCESS: 0, ACCES: 2, BADF: 8, EXIST: 20, FAULT: 21, INVAL: 28, IO: 29,
    ISDIR: 31, NOENT: 44, NOSYS: 52, NOTDIR: 54, NOTSUP: 58, PERM: 63,
  };

  const FILETYPE_DIRECTORY = 3;
  const FILETYPE_REGULAR_FILE = 4;

  const WHENCE_SET = 0, WHENCE_CUR = 1, WHENCE_END = 2;

  function createWasi(opts) {
    const decoder = new TextDecoder("utf-8");
    const memorySource = { mem: null };

    // ficheros virtuales: ruta -> Uint8Array
    const files = opts.files || {};
    const dirs = new Set(opts.dirs || []);
    for (const p of Object.keys(files)) {
      const parts = p.split("/");
      parts.pop();
      let cur = "";
      for (const part of parts) {
        if (!part) continue;
        cur = cur ? cur + "/" + part : part;
        dirs.add(cur);
      }
    }

    // tabla de descriptores
    let nextFd = 4;
    const fds = new Map();
    function addFd(entry) { const fd = nextFd++; fds.set(fd, entry); return fd; }

    // preopen del directorio "." (equivalente al cwd del proceso nativo)
    fds.set(3, { type: "dir", path: "." });

    function fdOf(fd) { return fds.get(fd | 0); }

    function errnoBadF(ptr) { return ERRNO.BADF; }

    function normPath(base, path) {
      let p = path;
      if (base && base !== "." && !p.startsWith("/")) p = base + "/" + p;
      const out = [];
      for (const part of p.split("/")) {
        if (!part || part === ".") continue;
        if (part === "..") { out.pop(); continue; }
        out.push(part);
      }
      return out.join("/");
    }

    function pathExists(path) {
      if (path === "" || path === ".") return "dir";
      if (dirs.has(path)) return "dir";
      if (files[path]) return "file";
      // rutas "..": el motor nativo prueba "../data/..." — en el navegador
      // no existe; se resuelve vacío (el motor cae al siguiente candidato).
      return null;
    }

    // ---- ABI helpers -------------------------------------------------------
    function dataView() { return new DataView(memorySource.mem.buffer); }
    function u8(ptr) { return new Uint8Array(memorySource.mem.buffer); }
    function readStr(ptr, len) { return decoder.decode(u8().subarray(ptr, ptr + len)); }
    function writeU32(ptr, v) { dataView().setUint32(ptr, v, true); }
    function writeU64(ptr, v) { dataView().setBigUint64(ptr, BigInt(v), true); }

    const wasi = {
      args_get(argvPtr, argvBufPtr) {
        writeU32(argvPtr, argvBufPtr);
        u8()[argvBufPtr] = 0;
        return ERRNO.SUCCESS;
      },
      args_sizes_get(argcPtr, argvBufSizePtr) {
        writeU32(argcPtr, 1);
        writeU32(argvBufSizePtr, 1);
        return ERRNO.SUCCESS;
      },
      environ_get(environPtr, environBufPtr) {
        writeU32(environPtr, environBufPtr);
        return ERRNO.SUCCESS;
      },
      environ_sizes_get(countPtr, sizePtr) {
        writeU32(countPtr, 0);
        writeU32(sizePtr, 0);
        return ERRNO.SUCCESS;
      },
      clock_time_get(id, precision, timePtr) {
        const ms = Date.now();
        writeU64(timePtr, BigInt(ms) * 1000000n);
        return ERRNO.SUCCESS;
      },
      random_get(bufPtr, bufLen) {
        const buf = u8().subarray(bufPtr, bufPtr + bufLen);
        if (opts.randomBytes) opts.randomBytes(buf);
        else for (let i = 0; i < buf.length; i++) buf[i] = (Math.random() * 256) | 0;
        return ERRNO.SUCCESS;
      },
      proc_exit(code) {
        if (opts.onExit) opts.onExit(code | 0);
        return ERRNO.SUCCESS;
      },
      fd_close(fd) {
        if (!fdOf(fd)) return ERRNO.BADF;
        fds.delete(fd | 0);
        return ERRNO.SUCCESS;
      },
      fd_write(fd, iovsPtr, iovsLen, nwrittenPtr) {
        const dv = dataView();
        let written = 0;
        const chunks = [];
        for (let i = 0; i < iovsLen; i++) {
          const base = iovsPtr + i * 8;
          const buf = dv.getUint32(base, true);
          const len = dv.getUint32(base + 4, true);
          if (len) {
            const bytes = u8().subarray(buf, buf + len);
            if (fd === 1 || fd === 2) chunks.push(decoder.decode(bytes));
            written += len;
          }
        }
        if (chunks.length && opts.onStdout) opts.onStdout(chunks.join(""));
        writeU32(nwrittenPtr, written);
        return ERRNO.SUCCESS;
      },
      fd_read(fd, iovsPtr, iovsLen, nreadPtr) {
        const e = fdOf(fd);
        if (!e) return ERRNO.BADF;
        if (e.type !== "file") return ERRNO.ISDIR;
        const dv = dataView();
        let read = 0;
        for (let i = 0; i < iovsLen; i++) {
          const base = iovsPtr + i * 8;
          const buf = dv.getUint32(base, true);
          const len = dv.getUint32(base + 4, true);
          const remain = e.data.length - e.offset;
          if (remain <= 0) break;
          const n = Math.min(len, remain);
          u8().set(e.data.subarray(e.offset, e.offset + n), buf);
          e.offset += n;
          read += n;
          if (n < len) break;
        }
        writeU32(nreadPtr, read);
        return ERRNO.SUCCESS;
      },
      fd_seek(fd, offsetLo, offsetHi, whence, newOffsetPtr) {
        const e = fdOf(fd);
        if (!e) return ERRNO.BADF;
        const offset = Number(BigInt.asIntN(64, (BigInt(offsetHi) << 32n) | BigInt(offsetLo >>> 0)));
        if (e.type === "file") {
          if (whence === WHENCE_SET) e.offset = offset;
          else if (whence === WHENCE_CUR) e.offset += offset;
          else if (whence === WHENCE_END) e.offset = e.data.length + offset;
          e.offset = Math.max(0, Math.min(e.offset, e.data.length));
        }
        writeU64(newOffsetPtr, e.type === "file" ? e.offset : 0);
        return ERRNO.SUCCESS;
      },
      fd_fdstat_get(fd, statPtr) {
        const e = fdOf(fd);
        if (!e) return ERRNO.BADF;
        const dv = dataView();
        dv.setUint8(statPtr, e.type === "dir" ? FILETYPE_DIRECTORY : FILETYPE_REGULAR_FILE);
        dv.setUint16(statPtr + 2, 0, true);
        dv.setBigUint64(statPtr + 8, 0xffffffffffffffffn, true);
        dv.setBigUint64(statPtr + 16, 0xffffffffffffffffn, true);
        return ERRNO.SUCCESS;
      },
      fd_prestat_get(fd, prestatPtr) {
        const e = fdOf(fd);
        if (!e || e.type !== "dir") return ERRNO.BADF;
        const dv = dataView();
        dv.setUint8(prestatPtr, 0); // tag: preopendir
        dv.setUint32(prestatPtr + 4, e.path.length, true);
        return ERRNO.SUCCESS;
      },
      fd_prestat_dir_name(fd, pathPtr, pathLen) {
        const e = fdOf(fd);
        if (!e || e.type !== "dir") return ERRNO.BADF;
        const bytes = new TextEncoder().encode(e.path);
        u8().set(bytes.subarray(0, pathLen), pathPtr);
        return ERRNO.SUCCESS;
      },
      path_open(dirfd, dirflags, pathPtr, pathLen, oflags, rightsBase, rightsInh, fdflags, openedPtr) {
        const dir = fdOf(dirfd);
        if (!dir) return ERRNO.BADF;
        const raw = readStr(pathPtr, pathLen);
        const path = normPath(dir.path === "." ? "" : dir.path, raw);
        const kind = pathExists(path);
        if (!kind) return ERRNO.NOENT;
        if (kind === "dir") {
          const fd = addFd({ type: "dir", path: path });
          writeU32(openedPtr, fd);
          return ERRNO.SUCCESS;
        }
        const fd = addFd({ type: "file", path: path, data: files[path], offset: 0 });
        writeU32(openedPtr, fd);
        return ERRNO.SUCCESS;
      },
      // Sin uso por el motor, pero WASI las define: se contestan con honestidad
      fd_fdstat_set_flags() { return ERRNO.NOSYS; },
      fd_sync() { return ERRNO.SUCCESS; },
      fd_datasync() { return ERRNO.SUCCESS; },
      sched_yield() { return ERRNO.SUCCESS; },
    };

    return {
      wasi,
      setMemory(mem) { memorySource.mem = mem; },
      filenames() { return Object.keys(files); },
    };
  }

  // -------------------------------------------------------------------------
  // Puente de RED: el motor pide una URL y el navegador es quien la descarga.
  // Se usa XMLHttpRequest SÍNCRONO porque el motor hace la petición de forma
  // bloqueante (como un navegador real cuando espera una respuesta). El
  // navegador solo permite leer respuestas del MISMO ORIGEN (o de servidores
  // que autoricen CORS): si no puede, el motor recibe un error y muestra su
  // propia página de error. No se disfraza: se devuelve el fallo real.
  // -------------------------------------------------------------------------
  function createHttpFetch(getMemory) {
    return function httpFetch(urlPtr, urlLen, methodPtr, methodLen,
                              bodyPtr, bodyLen, outDataPtr, outLenPtr) {
      const mem = getMemory();
      const dec = new TextDecoder();
      const bytes = new Uint8Array(mem.buffer);
      const url = dec.decode(bytes.subarray(urlPtr, urlPtr + urlLen));
      const method = dec.decode(bytes.subarray(methodPtr, methodPtr + methodLen)) || "GET";
      const body = bodyLen > 0 ? dec.decode(bytes.subarray(bodyPtr, bodyPtr + bodyLen)) : null;
      try {
        const xhr = new XMLHttpRequest();
        xhr.open(method, url, false); // síncrono: el motor espera la respuesta
        xhr.overrideMimeType && xhr.overrideMimeType("text/plain; charset=x-user-defined");
        if (method !== "GET" && method !== "HEAD") {
          xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
        }
        xhr.send(body);
        let text = xhr.responseText || "";
        // responseText pierde los bytes >127 al ser síncrono: se reconstruye
        const raw = new Uint8Array(text.length);
        for (let i = 0; i < text.length; i++) raw[i] = text.charCodeAt(i) & 0xff;

        const head = "HTTP/1.1 " + xhr.status + " " + (xhr.statusText || "OK") + "\r\n" +
                     "Content-Length: " + raw.length + "\r\n" +
                     "X-Nuby-Final-Url: " + (xhr.responseURL || url) + "\r\n\r\n";
        const headBytes = new TextEncoder().encode(head);
        const total = headBytes.length + raw.length;
        const ptr = _alloc(total);
        bytes.set(headBytes, ptr);
        bytes.set(raw, ptr + headBytes.length);
        new DataView(mem.buffer).setUint32(outDataPtr, ptr, true);
        new DataView(mem.buffer).setUint32(outLenPtr, total, true);
        return 0;
      } catch (e) {
        return -1;
      }
    };
  }

  // Reserva memoria dentro del wasm (export nuby_alloc). Se conecta al
  // instanciar el módulo.
  let _alloc = null;
  function setAlloc(fn) { _alloc = fn; }

  window.NubyWasi = { createWasi, createHttpFetch, setAlloc };
})();
