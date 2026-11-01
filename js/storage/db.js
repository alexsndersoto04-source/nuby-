/**
 * Persistencia en IndexedDB.
 *
 * Los proyectos (pistas, clips, ajustes) se guardan como JSON y el audio como
 * Float32Array. IndexedDB guarda binarios de forma nativa: no se convierte a
 * base64 ni se pierde calidad, y aguanta proyectos de varios minutos sin
 * reventar el almacenamiento de localStorage.
 */

const DB_NAME = 'nuby-studio';
const DB_VERSION = 1;
const STORE_PROJECTS = 'projects';
const STORE_BUFFERS = 'buffers';

let dbPromise = null;

export function openDb() {
  if (dbPromise) return dbPromise;
  dbPromise = new Promise((resolve, reject) => {
    if (!('indexedDB' in window)) {
      reject(new Error('Este navegador no soporta IndexedDB; no se podrán guardar proyectos.'));
      return;
    }
    const request = indexedDB.open(DB_NAME, DB_VERSION);
    request.onupgradeneeded = () => {
      const db = request.result;
      if (!db.objectStoreNames.contains(STORE_PROJECTS)) {
        const store = db.createObjectStore(STORE_PROJECTS, { keyPath: 'id' });
        store.createIndex('updatedAt', 'updatedAt');
      }
      if (!db.objectStoreNames.contains(STORE_BUFFERS)) {
        db.createObjectStore(STORE_BUFFERS, { keyPath: 'id' });
      }
    };
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error('No se pudo abrir la base de datos'));
  });
  return dbPromise;
}

function run(storeName, mode, action) {
  return openDb().then(
    (db) =>
      new Promise((resolve, reject) => {
        const transaction = db.transaction(storeName, mode);
        const store = transaction.objectStore(storeName);
        let result;
        try {
          result = action(store);
        } catch (error) {
          reject(error);
          return;
        }
        transaction.oncomplete = () => resolve(result);
        transaction.onerror = () => reject(transaction.error);
        transaction.onabort = () => reject(transaction.error);
      })
  );
}

/** Guarda el proyecto completo (metadatos). */
export function saveProject(project) {
  return run(STORE_PROJECTS, 'readwrite', (store) => store.put(project));
}

/** Lista de proyectos con su información básica, del más reciente al más viejo. */
export async function listProjects() {
  const db = await openDb();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_PROJECTS, 'readonly');
    const store = tx.objectStore(STORE_PROJECTS);
    const request = store.getAll();
    request.onsuccess = () => {
      const items = (request.result ?? []).map((p) => ({
        id: p.id,
        name: p.name,
        updatedAt: p.updatedAt,
        createdAt: p.createdAt,
        duration: p.duration ?? 0,
        trackCount: p.tracks?.length ?? 0,
      }));
      items.sort((a, b) => (b.updatedAt ?? 0) - (a.updatedAt ?? 0));
      resolve(items);
    };
    request.onerror = () => reject(request.error);
  });
}

export async function loadProject(id) {
  const db = await openDb();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_PROJECTS, 'readonly');
    const request = tx.objectStore(STORE_PROJECTS).get(id);
    request.onsuccess = () => resolve(request.result ?? null);
    request.onerror = () => reject(request.error);
  });
}

/** Borra un proyecto y todos sus audios. */
export async function deleteProject(id) {
  const project = await loadProject(id);
  await run(STORE_PROJECTS, 'readwrite', (store) => store.delete(id));
  if (project?.clips?.length) {
    const ids = [...new Set(project.clips.map((c) => c.bufferId))];
    await Promise.all(ids.map((bufferId) => deleteBuffer(bufferId).catch(() => {})));
  }
}

/** Guarda un audio (Float32Array por canal). */
export function putBuffer(record) {
  return run(STORE_BUFFERS, 'readwrite', (store) => store.put(record));
}

export async function getBuffer(id) {
  const db = await openDb();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_BUFFERS, 'readonly');
    const request = tx.objectStore(STORE_BUFFERS).get(id);
    request.onsuccess = () => resolve(request.result ?? null);
    request.onerror = () => reject(request.error);
  });
}

export function deleteBuffer(id) {
  return run(STORE_BUFFERS, 'readwrite', (store) => store.delete(id));
}

export async function bufferExists(id) {
  const record = await getBuffer(id);
  return Boolean(record);
}

/** Borra audios que ningún proyecto referencia (limpieza de espacio). */
export async function collectGarbage() {
  const projects = await listProjects();
  const used = new Set();
  for (const summary of projects) {
    const project = await loadProject(summary.id);
    for (const clip of project?.clips ?? []) used.add(clip.bufferId);
  }
  const db = await openDb();
  const allIds = await new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_BUFFERS, 'readonly');
    const request = tx.objectStore(STORE_BUFFERS).getAllKeys();
    request.onsuccess = () => resolve(request.result ?? []);
    request.onerror = () => reject(request.error);
  });
  const orphans = allIds.filter((id) => !used.has(id));
  await Promise.all(orphans.map((id) => deleteBuffer(id)));
  return orphans.length;
}

/** Espacio usado y disponible (si el navegador lo informa). */
export async function storageEstimate() {
  if (!navigator.storage?.estimate) return null;
  try {
    const { usage, quota } = await navigator.storage.estimate();
    return { usage, quota };
  } catch {
    return null;
  }
}

/** ¿La app está instalada / en modo autónomo? */
export function isStandalone() {
  return window.matchMedia?.('(display-mode: standalone)').matches || navigator.standalone === true;
}
