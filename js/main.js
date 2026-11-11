/**
 * Punto de entrada de Ask.
 */
import { StudioApp } from './ui/app.js';

const app = new StudioApp();
window.ask = app; // útil para depurar desde la consola

app.bootPromise = app.boot().catch((error) => {
  console.error(error);
  const container = document.getElementById('app');
  if (!container) return;
  container.setAttribute('aria-busy', 'false');
  const box = document.createElement('div');
  box.style.cssText =
    'position:fixed;inset:0;display:flex;align-items:center;justify-content:center;background:#101315;padding:24px;z-index:999';
  box.innerHTML = `<div style="max-width:520px;font-family:system-ui;color:#dfe4e8;line-height:1.6">
      <h1 style="font-size:17px;margin:0 0 8px">No se pudo iniciar el estudio</h1>
      <p style="color:#a3adb6;margin:0 0 12px">Se produjo un error al arrancar la interfaz:</p>
      <pre style="background:#161a1e;border:1px solid #262d34;border-radius:6px;padding:12px;overflow:auto;font-size:12px;color:#d8a08f"></pre>
      <p style="color:#7f8a93;font-size:12px;margin-top:12px">Abre la consola del navegador para ver el detalle completo.</p>
    </div>`;
  box.querySelector('pre').textContent = error?.stack ?? String(error);
  document.body.appendChild(box);
});
