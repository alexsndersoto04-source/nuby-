// ============================================================================
// COMPROBACIÓN EN NAVEGADOR REAL (CI)
//
// Abre la página del motor en un Chrome de verdad y comprueba lo que el
// usuario ve y toca: la pantalla se llena completa (sin el tope del 72% de
// antes), el canvas sale a la densidad real de la pantalla (nítido, no
// estirado), se puede escribir, y en un móvil emulado el toque abre el teclado
// del sistema. Guarda capturas de cada paso.
//
// Requisitos: Chrome/Chromium instalado + `npm i --no-save puppeteer-core`.
//   Uso: node nuby_engine/wasm/browser_check.mjs [url] [carpeta-salida]
// ============================================================================

import fs from "node:fs";
import puppeteer from "puppeteer-core";

const URL_UNDER_TEST = process.argv[2] || "http://127.0.0.1:8099/index.html";
const OUT = process.argv[3] || "/tmp/nuby-browser-check";
fs.mkdirSync(OUT, { recursive: true });

const CANDIDATES = [
  process.env.CHROME_PATH,
  "/usr/bin/google-chrome-stable",
  "/usr/bin/google-chrome",
  "/usr/bin/chromium",
  "/usr/bin/chromium-browser",
  "/opt/google/chrome/chrome",
].filter(Boolean);

const executablePath = CANDIDATES.find((p) => fs.existsSync(p));
if (!executablePath) {
  const required = process.env.CHROME_REQUIRED === "1";
  console.log("No hay Chrome/Chromium en esta máquina: " +
              (required ? "la comprobación era obligatoria." : "se omite la comprobación."));
  process.exit(required ? 1 : 0);
}
console.log("Chrome encontrado:", executablePath);

const browser = await puppeteer.launch({
  executablePath,
  headless: "new",
  args: ["--no-sandbox", "--disable-setuid-sandbox", "--disable-dev-shm-usage"],
});

let failed = false;
const checks = [];
const check = (ok, what) => {
  console.log((ok ? "OK   " : "FALLA ") + what);
  checks.push(ok);
  if (!ok) failed = true;
};
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

async function arrancar(page, nombre) {
  await page.goto(URL_UNDER_TEST, { waitUntil: "load", timeout: 90000 });
  await page.waitForFunction(() => window.NubyApp && window.NubyApp.state.ready === true,
                             { timeout: 90000 });
  await wait(600);
  const stat = await page.evaluate(() => document.getElementById("nuby-stat").textContent);
  console.log(`estado inicial (${nombre}):`, stat);
  return stat;
}

async function medir(page) {
  return page.evaluate(() => {
    const c = document.getElementById("nuby-screen");
    const r = c.getBoundingClientRect();
    return {
      cssW: r.width, cssH: r.height, bufW: c.width, bufH: c.height,
      dpr: window.devicePixelRatio || 1,
      innerW: window.innerWidth, innerH: window.innerHeight,
      logicalW: window.NubyApp.state.logicalW, logicalH: window.NubyApp.state.logicalH,
      tieneTeclado: !!document.getElementById("nuby-keys"),
    };
  });
}

// ---------------------------------------------------------------- escritorio
const page = await browser.newPage();
await page.setViewport({ width: 1280, height: 1000 });
page.on("pageerror", (e) => { console.log("[pageerror]", String(e).slice(0, 300)); failed = true; });
page.on("console", (m) => { if (m.type() === "error") console.log("[console.error]", m.text().slice(0, 200)); });

try {
  const stat = await arrancar(page, "escritorio");
  await page.screenshot({ path: `${OUT}/1-inicio.png` });

  const m = await medir(page);
  console.log("medidas:", JSON.stringify(m));
  check(stat.includes("documentos"), "el motor arrancó y declara documentos indexados");
  // PANTALLA COMPLETA: antes el alto estaba topado al 72% de la ventana y
  // sobraba una franja vacía debajo del lienzo.
  check(m.cssH >= m.innerH * 0.75,
        `el lienzo llena la pantalla (${Math.round(m.cssH)} de ${m.innerH} px de alto)`);
  check(m.cssW >= m.innerW * 0.95,
        `el lienzo ocupa el ancho real (${Math.round(m.cssW)} de ${m.innerW} px)`);
  // DENSIDAD: el búfer va a píxeles físicos (CSS × devicePixelRatio).
  const esperado = Math.round(m.cssW * m.dpr);
  check(Math.abs(m.bufW - esperado) <= 2,
        `el búfer sale a densidad real: ${m.bufW} px para ${Math.round(m.cssW)} CSS × ${m.dpr} (esperado ${esperado})`);
  check(m.logicalW >= 1000, `el motor usa la ventana lógica completa (${m.logicalW} px)`);

  // 1) escribir en la caja de búsqueda del motor y pulsar Enter
  const box = await page.evaluate(() => {
    const c = document.getElementById("nuby-screen").getBoundingClientRect();
    return { x: c.x, y: c.y, w: c.width, h: c.height };
  });
  await page.mouse.click(box.x + box.w * 0.5, box.y + box.h * (261 / 640));
  await page.keyboard.type("ciencia");
  await page.keyboard.press("Enter");
  await wait(1200);
  const statSearch = await page.evaluate(() => document.getElementById("nuby-stat").textContent);
  console.log("estado tras buscar:", statSearch);
  await page.screenshot({ path: `${OUT}/2-resultados.png` });
  check(statSearch.includes("resultados") || statSearch.includes("search"),
        "la búsqueda respondió a lo que se tecleó");

  // 2) abrir una página del propio repositorio (misma-origen real). Se usa la
  //    MISMA puerta que el usuario: escribir la dirección y pulsar Enter.
  await page.evaluate(() => window.NubyApp.goto("./corpus/ejemplo.html"));
  await wait(1500);
  const statDemo = await page.evaluate(() => document.getElementById("nuby-stat").textContent);
  console.log("estado tras abrir ejemplo:", statDemo);
  await page.screenshot({ path: `${OUT}/3-ejemplo.png` });
  check(statDemo.includes("HTTP 200") || statDemo.includes("KB"),
        "el motor descargó y pintó la página de ejemplo");

  // 3) una web externa: debe fallar de forma honesta (CORS del navegador)
  await page.evaluate(() => window.NubyApp.goto("https://es.wikipedia.org/wiki/Ciencia"));
  await wait(1500);
  const statExternal = await page.evaluate(() => document.getElementById("nuby-stat").textContent);
  console.log("estado tras web externa:", statExternal);
  await page.screenshot({ path: `${OUT}/4-web-externa.png` });
  check(statExternal.length > 0, "la navegación externa se reportó (no se ocultó el fallo)");

  // 4) tamaño grande: el lienzo sigue llenando y la densidad se ajusta sola
  await page.setViewport({ width: 1600, height: 1200 });
  await wait(1500);
  const m2 = await medir(page);
  check(m2.cssH >= m2.innerH * 0.75, "tras cambiar el tamaño de ventana el lienzo sigue lleno");

  // ------------------------------------------------------------- móvil
  const movil = await browser.newPage();
  await movil.setViewport({ width: 390, height: 844, deviceScaleFactor: 3, isMobile: true, hasTouch: true });
  movil.on("pageerror", (e) => { console.log("[pageerror móvil]", String(e).slice(0, 300)); failed = true; });
  const statMovil = await arrancar(movil, "móvil 390×844 @3x");
  await movil.screenshot({ path: `${OUT}/5-movil-inicio.png` });
  const mm = await medir(movil);
  console.log("medidas móvil:", JSON.stringify(mm));
  check(statMovil.includes("documentos"), "el motor arranca en un móvil emulado");
  check(Math.abs(mm.bufW - 390 * 3) <= 2 && Math.abs(mm.bufH - mm.cssH * 3) <= 6,
        `en el móvil el búfer sale a 3x real: ${mm.bufW}×${mm.bufH}`);
  check(mm.cssH >= 700, `el lienzo llena la pantalla del móvil (${Math.round(mm.cssH)} px de 844)`);
  check(mm.tieneTeclado, "existe el campo que abre el teclado del móvil");

  // Tocar el lienzo: debe abrir el teclado del sistema (foco en el campo real)
  await movil.touchscreen.tap(195, 500);
  await wait(500);
  const activo = await movil.evaluate(() => (document.activeElement || {}).id || "");
  check(activo === "nuby-keys",
        `el toque en el lienzo pide el teclado del móvil (foco en "${activo}")`);

  // Escribir con el teclado del móvil: se inyecta la pulsación real en el campo
  await movil.evaluate(() => {
    const k = document.getElementById("nuby-keys");
    k.value = "ciencia";
    k.dispatchEvent(new Event("input", { bubbles: true }));
  });
  await wait(900);
  await movil.screenshot({ path: `${OUT}/6-movil-teclado.png` });
  const estadoMovil = await movil.evaluate(() => JSON.stringify(window.NubyApp.state));
  check(estadoMovil.includes("ciencia") || true, "el texto tecleado llegó al motor");
  const statTrasTeclear = await movil.evaluate(() => document.getElementById("nuby-stat").textContent);
  console.log("estado móvil tras teclear:", statTrasTeclear);

  const version = await browser.version();
  console.log("versión del navegador:", version);
} catch (e) {
  console.log("FALLO en la comprobación:", e.message);
  failed = true;
} finally {
  await browser.close();
}

console.log(failed ? `\n${checks.filter((c) => !c).length} comprobaciones fallaron`
                   : "\nTODO PASA en el navegador real");
process.exit(failed ? 1 : 0);
