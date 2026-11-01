/**
 * Servidor estático de desarrollo.
 *
 * No procesa audio ni guarda nada: la app es 100 % cliente. Sirve para abrirla
 * en localhost (necesario para que el navegador permita el micrófono y los
 * módulos ES).
 *
 * Uso: node server.js [puerto]
 */

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.dirname(fileURLToPath(import.meta.url));
const PORT = Number(process.argv[2] ?? process.env.PORT ?? 4173);
const HOST = process.env.HOST ?? '0.0.0.0';

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.webmanifest': 'application/manifest+json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.ico': 'image/x-icon',
  '.wav': 'audio/wav',
  '.mp3': 'audio/mpeg',
  '.map': 'application/json; charset=utf-8',
};

const server = http.createServer((req, res) => {
  const requestUrl = new URL(req.url, `http://${req.headers.host ?? 'localhost'}`);
  let pathname = decodeURIComponent(requestUrl.pathname);
  if (pathname.endsWith('/')) pathname += 'index.html';

  const filePath = path.join(ROOT, path.normalize(pathname).replace(/^(\.\.[/\\])+/, ''));
  if (filePath !== ROOT && !filePath.startsWith(ROOT + path.sep)) {
    res.writeHead(403).end('Prohibido');
    return;
  }

  fs.stat(filePath, (error, stats) => {
    if (error || !stats.isFile()) {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('404 — No encontrado');
      return;
    }

    const type = MIME[path.extname(filePath).toLowerCase()] ?? 'application/octet-stream';
    const headers = {
      'Content-Type': type,
      // Sin caché: al editar, el navegador siempre ve la versión nueva.
      'Cache-Control': 'no-cache, must-revalidate',
      // Necesario para que AudioWorklet pueda cargarse desde este origen.
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Resource-Policy': 'same-origin',
    };

    const range = req.headers.range;
    if (range && /^bytes=/.test(range)) {
      const [startText, endText] = range.replace('bytes=', '').split('-');
      const start = Number.parseInt(startText, 10) || 0;
      const end = endText ? Math.min(Number.parseInt(endText, 10), stats.size - 1) : stats.size - 1;
      if (start >= stats.size) {
        res.writeHead(416, { 'Content-Range': `bytes */${stats.size}` }).end();
        return;
      }
      res.writeHead(206, {
        ...headers,
        'Content-Range': `bytes ${start}-${end}/${stats.size}`,
        'Content-Length': end - start + 1,
        'Accept-Ranges': 'bytes',
      });
      fs.createReadStream(filePath, { start, end }).pipe(res);
      return;
    }

    res.writeHead(200, { ...headers, 'Content-Length': stats.size, 'Accept-Ranges': 'bytes' });
    fs.createReadStream(filePath).pipe(res);
  });
});

server.listen(PORT, HOST, () => {
  console.log(`Nuby Studio disponible en http://localhost:${PORT} (escuchando en ${HOST}:${PORT})`);
  console.log('Abre esa dirección en Chrome, Edge o Firefox. El micrófono requiere localhost o HTTPS.');
});
