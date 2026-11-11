/**
 * Service worker: permite usar el estudio sin conexión y abrirlo como app.
 * Estrategia: cache-first para los archivos de la app, red primero para las
 * navegaciones (para recibir actualizaciones sin quedarse pegado).
 */

const VERSION = 'ask-v1';
const SHELL = [
  './',
  './index.html',
  './css/app.css',
  './js/main.js',
  './js/state.js',
  './js/util/format.js',
  './js/storage/db.js',
  './js/audio/engine.js',
  './js/audio/track-chain.js',
  './js/audio/transport.js',
  './js/audio/recorder.js',
  './js/audio/metronome.js',
  './js/audio/render.js',
  './js/audio/mp3.js',
  './js/audio/wav.js',
  './js/audio/lufs.js',
  './js/audio/peaks.js',
  './js/audio/impulse.js',
  './js/audio/worklets/recorder-processor.js',
  './js/audio/worklets/gate-processor.js',
  './js/audio/worklets/limiter-processor.js',
  './js/audio/worklets/lufs-processor.js',
  './js/ui/app.js',
  './js/ui/dom.js',
  './js/ui/strips.js',
  './js/ui/timeline.js',
  './js/ui/inspector.js',
  './js/ui/dialogs.js',
  './js/vendor/lamejs.js',
  './manifest.webmanifest',
  './assets/icon.svg',
  './assets/icon-192.png',
  './assets/icon-512.png',
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches
      .open(VERSION)
      .then((cache) => cache.addAll(SHELL))
      .then(() => self.skipWaiting())
      .catch((error) => console.warn('No se pudo precachear:', error))
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches
      .keys()
      .then((keys) => Promise.all(keys.filter((key) => key !== VERSION).map((key) => caches.delete(key))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', (event) => {
  const request = event.request;
  if (request.method !== 'GET') return;

  const url = new URL(request.url);
  if (url.origin !== self.location.origin) return;

  if (request.mode === 'navigate') {
    event.respondWith(
      fetch(request)
        .then((response) => {
          const copy = response.clone();
          caches.open(VERSION).then((cache) => cache.put('./index.html', copy)).catch(() => {});
          return response;
        })
        .catch(() => caches.match('./index.html').then((cached) => cached ?? caches.match('./')))
    );
    return;
  }

  event.respondWith(
    caches.match(request).then((cached) => {
      if (cached) return cached;
      return fetch(request)
        .then((response) => {
          if (response.ok && response.type === 'basic') {
            const copy = response.clone();
            caches.open(VERSION).then((cache) => cache.put(request, copy)).catch(() => {});
          }
          return response;
        })
        .catch(() => cached);
    })
  );
});
