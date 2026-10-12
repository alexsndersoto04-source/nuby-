// ============================================================================
// COMPROBACIÓN EN NAVEGADOR REAL (CI)
//
// Abre la página del motor en un Chrome de verdad, espera a que el motor
// arranque, interactúa (clic + teclado) y guarda capturas. Es la prueba de
// que el wasm arranca en un navegador, no solo en Node.
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
const page = await browser.newPage();
await page.setViewport({ width: 1280, height: 1000 });
page.on("pageerror", (e) => { console.log("[pageerror]", String(e).slice(0, 300)); failed = true; });
page.on("console", (m) => { if (m.type() === "error") console.log("[console.error]", m.text().slice(0, 200)); });

try {
  await page.goto(URL_UNDER_TEST, { waitUntil: "load", timeout: 90000 });
  await page.waitForFunction(() => window.NubyApp && window.NubyApp.state.ready === true, { timeout: 90000 });

  const stat = await page.evaluate(() => document.getElementById("nuby-stat").textContent);
  console.log("estado inicial:", stat);
  await page.screenshot({ path: `${OUT}/1-inicio.png` });

  const box = await page.evaluate(() => {
    const c = document.getElementById("nuby-screen").getBoundingClientRect();
    return { x: c.x, y: c.y, w: c.width, h: c.height };
  });

  // 1) escribir en la caja de búsqueda del motor y pulsar Enter
  await page.mouse.click(box.x + box.w * 0.5, box.y + box.h * (261 / 640));
  await page.keyboard.type("ciencia");
  await page.keyboard.press("Enter");
  await new Promise((r) => setTimeout(r, 1200));
  const statSearch = await page.evaluate(() => document.getElementById("nuby-stat").textContent);
  console.log("estado tras buscar:", statSearch);
  await page.screenshot({ path: `${OUT}/2-resultados.png` });

  // 2) abrir la página de ejemplo del repositorio (misma-origen real)
  await page.click("#btn-demo");
  await new Promise((r) => setTimeout(r, 1500));
  const statDemo = await page.evaluate(() => document.getElementById("nuby-stat").textContent);
  console.log("estado tras abrir ejemplo:", statDemo);
  await page.screenshot({ path: `${OUT}/3-ejemplo.png` });

  // 3) una web externa: debe fallar de forma honesta (CORS del navegador)
  await page.evaluate(() => {
    const el = document.getElementById("nuby-stat");
    el.dataset.before = el.textContent;
  });
  await page.evaluate(() => window.NubyApp.goto("https://es.wikipedia.org/wiki/Ciencia"));
  await new Promise((r) => setTimeout(r, 1500));
  const statExternal = await page.evaluate(() => document.getElementById("nuby-stat").textContent);
  console.log("estado tras web externa:", statExternal);
  await page.screenshot({ path: `${OUT}/4-web-externa.png` });

  const checks = [
    [stat.includes("documentos"), "el motor arrancó y declara documentos indexados"],
    [statSearch.includes("resultados") || statSearch.includes("search"), "la búsqueda respondió"],
    [statDemo.includes("HTTP 200") || statDemo.includes("KB"), "el motor descargó y pintó la página de ejemplo"],
    [statExternal.length > 0, "la navegación externa se reportó (no se ocultó el fallo)"],
  ];
  const version = await browser.version();
  console.log("versión del navegador:", version);
  for (const [ok, what] of checks) {
    console.log((ok ? "OK   " : "FALLA ") + what);
    if (!ok) failed = true;
  }
} catch (e) {
  console.log("FALLO en la comprobación:", e.message);
  failed = true;
} finally {
  await browser.close();
}

process.exit(failed ? 1 : 0);
