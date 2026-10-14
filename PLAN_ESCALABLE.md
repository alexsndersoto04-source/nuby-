# PLAN NUBY — motor de búsqueda real, escalable y sin humo

> Este plan sustituye al documento anterior de despliegue como guía de crecimiento.
> Regla de oro: **cada número aquí es medido hoy o estimado con el método a la vista**.
> Si algo no se puede, se dice; si algo pide tarjeta bancaria, se marca y queda fuera.

---

## 0. Las tres reglas de este plan

1. **$0 real, sin tarjeta.** Todo lo que está en el camino principal funciona con lo que ya
   tienes: una cuenta de GitHub. Las piezas que piden tarjeta están marcadas y **NO** son
   parte del plan (R2, Workers Paid, cualquier servidor pagado).
2. **Nada simulado.** Cada fase se cierra con un número que se puede contar (páginas,
   dominios, milisegundos, bytes). Si no se puede medir, no es un hito.
3. **La línea honesta no se mueve.** Este plan NO abre YouTube, Facebook, Gmail ni bancos.
   Eso no lo arregla nada de aquí abajo: es el muro real del motor y se queda escrito.

---

## 1. La idea central: cuatro trabajos separados

El Nuby actual hace **todo en una máquina**: rastrea, indexa, sirve y pinta. Por eso no
escala: si crece, esa única máquina se cae o se acaba el plan gratuito.

Este plan separa los cuatro trabajos y pone cada uno donde es gratis, para siempre:

| Trabajo | Dónde vive | Por qué ahí |
|---|---|---|
| 1. Rastrear la web | **GitHub Actions** | Es la única máquina gratuita y sin tarjeta con internet abierto de verdad |
| 2. Construir el índice | **GitHub Actions** | Mismo sitio: trabajo por lotes, no necesita estar despierto |
| 3. Servir el índice | **GitHub Pages** (archivos estáticos) | Ancho de banda gratis, rompe el problema de "servidor que se duerme" |
| 4. Buscar y pintar | **El navegador del visitante** (Nuby en WebAssembly) | El músculo lo pone quien visita: cero CPU de servidor, escala sin límite de máquina |

**Consecuencia:** no hay servidor que pagar, no hay máquina que se duerma, y el
buscador puede atender a mucha más gente que hoy sin que tú pagues nada.

**Lo que esto SÍ cambia:** el techo pasa de "un servidor pequeño" a "los límites de los
planes gratuitos". **Lo que NO cambia:** el motor sigue sin poder con YouTube/Facebook/banco.

---

## 2. Las piezas, con sus límites reales (verificados 2026-10-13)

| Pieza | Límite gratuito | ¿Pide tarjeta? | Rol en Nuby |
|---|---|---|---|
| **GitHub Actions** | Minutos ilimitados en repos públicos; 6 h por trabajo; cron mínimo 5 min | No | Rastrear + indexar |
| **GitHub Pages** | 1 GB de sitio (recomendado); 100 GB/mes de ancho de banda (límite blando) | No | Servir índice + app Nuby |
| **Cloudflare Workers Free** | 100.000 peticiones/día; 10 ms de CPU por petición | No (Cloudflare no publica lista oficial, pero es el candidato sin tarjeta) | Opcional: modo vivo (abrir páginas) |
| **Cloudflare D1 Free** | 5 GB; 5 M filas leídas/día; 100 k escritas/día | No (misma salvedad) | Opcional: cola de rastreo (fase 2+) |
| **Cloudflare R2** | 10 GB y egreso gratis… **pero pide tarjeta para activarlo** | **SÍ → fuera del plan** | — |
| **Cloudflare Workers Paid** | $5/mes | **SÍ** | Techo: documentado, no parte del camino |
| **Render / similares** | Duerme a los 15 min, disco efímero | No | Solo para "abrir páginas vivas" (fase 3), no para buscar |
| **Navegador del visitante** | Ilimitado | — | Buscar (BM25) y pintar (motor Nuby) |

**Dato clave:** el buscador completo **no necesita Cloudflare para nada**. Cloudflare es
acelerador opcional (modo vivo). El camino principal es solo GitHub, y ahí no hay tarjeta
en ningún paso.

### Trampa conocida y cómo la esquivamos
Los workflows programados de un repo público **se desactivan solos a los 60 días sin
actividad en el repo** (GitHub lo hace en silencio). Mitigación: nuestro rastreador
**commitea sus propios resultados** → genera actividad → el reloj se reinicia solo.
La actividad no es "truco": es el fruto real del rastreo.

---

## 3. El flujo completo, paso a paso

```
  ┌─ cada N horas (cron de GitHub Actions) ────────────────────────────┐
  │  1. El rastreador despierta en la máquina de GitHub (¡internet!).  │
  │  2. Lee su "frontera": lista de URLs por visitar + huella de las    │
  │     ya vistas (se guarda como archivo en el repo).                  │
  │  3. Pide robots.txt de cada dominio, respeta Crawl-delay y va a 1   │
  │     página por segundo por dominio (nunca martillea un sitio).      │
  │  4. Guarda por cada página: URL, título, resumen corto, dominio,     │
  │     idioma y firma de contenido (basura). NO guarda la página entera.│
  │  5. Saca enlaces nuevos (normalizados) y los añade a la frontera.   │
  └────────────────────────────────────────────────────────────────────┘
                              │
  ┌─ siguiente trabajo en la misma máquina ────────────────────────────┐
  │  6. Construye el índice invertido y lo parte en trozos (shards).    │
  │  7. Escribe los archivos del índice en el sitio publicado (Pages).  │
  └────────────────────────────────────────────────────────────────────┘
                              │
  ┌─ visita de una persona ────────────────────────────────────────────┐
  │  8. Abre la página de Nuby (Pages). El motor Nuby (wasm) se carga.  │
  │  9. Teclea "inteligencia artificial" y pulsa Enter.                 │
  │ 10. Su propio navegador pide solo los 2-4 trozos de índice que      │
  │     corresponden a esas palabras.                                   │
  │ 11. El BM25 que ya existe dentro del motor (search_index.hpp)       │
  │     puntúa y ordena EN SU MÁQUINA.                                  │
  │ 12. La página de resultados la pinta el motor Nuby, píxel a píxel.  │
  └────────────────────────────────────────────────────────────────────┘
```

Un paso extra, opcional (fase 3): si el visitante pulsa un resultado, un pequeño servicio
intermedio descarga esa página y se la pasa al motor para pintarla en vivo.

---

## 4. Cómo se guarda el índice (esta es la parte que hace que escale)

**El problema del índice normal:** un índice invertido completo de 1 millón de páginas no
cabe en el plan gratuito y obligaría a descargar megabytes por consulta.

**La solución (diseño, no promesa):** guardar para cada palabra **solo sus mejores 100
resultados precalculados** (documento + frecuencia + si la palabra está en el título).
Es exactamente lo que hace falta para una búsqueda web: nadie necesita los 40.000
documentos de "de", necesita los 10 mejores de la consulta.

- Los documentos llevan **número**, no URL: 4 bytes por acierto.
- El índice se parte en **N trozos por palabra** (hash de la palabra), así una consulta
  descarga solo 2-4 archivos pequeños.
- Archivos **comprimidos con gzip** y **con nombre basado en su contenido**: si un trozo
  no cambió, no se reescribe ni se duplica en el historial de Git.
- Solo se guardan **resúmenes cortos** (200-400 caracteres), nunca la página completa:
  menos peso, menos riesgo legal y el usuario siempre va al sitio original.

**Estimación (método a la vista, a medir en Fase 0):**

| Tamaño | Índice estimado | ¿Cabe en Pages (1 GB)? |
|---|---|---|
| 10.000 páginas | ~15 MB | Sí, de sobra |
| 100.000 páginas | ~150 MB | Sí |
| 500.000 páginas | ~750 MB | Sí, cómodo |
| 1.000.000 páginas | ~1,5 GB | Al filo → trocear en 2 repos espejo |

**Coste por consulta:** ~50-300 KB → con 100 GB/mes de Pages caben del orden de
**350.000 a 2.000.000 de búsquedas al mes**. Muy por encima de lo que hoy necesitas.

---

## 5. La escalera: qué pasa a cada tamaño

| Escala | Qué se necesita | Dinero | Qué se rompe primero |
|---|---|---|---|
| 10 k páginas | Solo GitHub | $0 | Nada |
| 100 k páginas | Solo GitHub | $0 | Nada (calidad del índice es el trabajo real) |
| 500 k páginas | Solo GitHub | $0 | El trabajo de limpieza (spam, duplicados) |
| 1 M páginas | 2 repos espejo (2× Pages) | $0 | Limpieza + tiempo de rastreo |
| 3-5 M páginas | Espejos + compresión manual + CDN extra | $0 | Gestión cada vez más manual |
| 10 M+ | Infraestructura real (R2, Workers Paid, servidores) | **$5-50/mes** | Aquí sí hay que pagar, y pide tarjeta |

**Lectura honesta:** gratis y sin tarjeta llegas hasta el millón de páginas (y su
correspondiente tráfico). Después hay una pared que se llama "dinero". No hay truco que
la rodee; lo único que se puede hacer es **llegar al millón primero** y decidir entonces.

**Y la verdad incómoda que va encima de todo esto:** llegar al millón de páginas NO te da
usuarios. Crecer el índice es trabajo técnico (se resuelve); conseguir visitas es una
pelea aparte (no se resuelve con ingeniería).

---

## 6. Fases, con hitos que se cuentan con un dedo

### Fase 0 — El rastreador honesto (semana 1)
Trabajo: respetar `robots.txt` y `Crawl-delay`, identificarse con contacto, semillas
variadas (noticias, blogs, documentación, universidades, foros, wikis no-Wikipedia,
sitios personales), reutilizar el rastreo que ya está tirado en `nuby_massive.db`
(1.000 páginas + 7.962 entradas multimedia), y correr todo en Actions con cron.
- ✅ Hito: **≥ 50.000 páginas, ≥ 5.000 dominios distintos, < 30% de Wikipedia.**
- Verificación: contar líneas del archivo publicado y listar dominios únicos. Sin simular.

### Fase 1 — Índice por trozos + búsqueda en el navegador (semanas 2-3)
Trabajo: el formato de la sección 4, el indexador en Actions, publicar los trozos en
Pages, y que el motor Nuby (que ya tiene BM25 dentro) los consuma y pinte los resultados.
- ✅ Hito: buscar una palabra rara y **encontrar una página que no sea Wikipedia**,
  pintada por el motor Nuby, en < 1 s, con < 300 KB descargados.
- Verificación: la búsqueda se hace en un navegador real y se mide en la pestaña Red.

### Fase 2 — Calidad y crecimiento (semanas 4-8)
Trabajo: filtrar basura y duplicados, deduplicar por firma de contenido, diversificar
dominios (que un sitio no acapare el índice), mejorar ranking (título, frescura, enlaces
entrantes), redactar el párrafo de cada resultado.
- ✅ Hito: **≥ 300.000 páginas** y que las 10 primeras posiciones de una búsqueda normal
  sean de 10 dominios distintos y ninguno sea basura.

### Fase 3 — Abrir páginas vivas (semana 8+)
Trabajo: un servicio fino que descarga una página y se la pasa al motor para pintarla
(soluciona el bloqueo entre sitios que hoy tiene la versión de navegador). Con límite de
uso, respetando `robots.txt`, y sin descargar imágenes pesadas.
- ✅ Hito: pulsar un resultado y ver la página real pintada por Nuby.

### Fase 4 — Techo y decisión (cuando toque)
Al acercarse a 1 GB o 100 GB/mes: activar espejos (más repos), o aceptar el muro de $5/mes.
- ✅ Hito: tener los números reales en la mano para decidir, no antes.

---

## 7. Lo que este proyecto NUNCA va a ser (la línea honesta)

- **No** abrirá YouTube, Facebook, Instagram ni Gmail: son aplicaciones modernas, no páginas.
- **No** procesará bancos ni contraseñas. **Nunca escribas tu clave en un navegador hecho en casa.**
- **No** reproducirá video ni audio (no hay decodificadores).
- **No** mostrará la mayoría de las fotos (JPEG y GIF no están soportados; PNG sí, con decodificador propio).
- **No** competirá con Google en cobertura. Ese barco no existe en esta ruta.
- **No** tendrá usuarios por sí solo. Eso es trabajo aparte del técnico.

Lo que **SÍ** es, y es real: un buscador independiente, propio, con rastreo e índice
reales, que crece solo, gratis, y que pinta su interfaz con su propio motor.

---

## 8. Riesgos reales (sin adornos)

| Riesgo | Qué tan grave | Qué hacemos |
|---|---|---|
| Los planes gratuitos cambian (Fly.io y Koyeb ya cerraron el suyo) | Alto a largo plazo | Arquitectura sin dependencia de un solo servicio: el índice son archivos; se mudan a donde sea |
| Límite de uso de Actions (fair use, no abusar) | Medio | Rastrear por ventanas razonables, no 20 rastreadores 24/7 |
| Legal: robots.txt, derechos de autor, datos personales | Alto | Respetar robots, guardar resúmenes y no el texto completo, no indexar datos privados |
| Mal uso del "modo vivo" como proxy abierto | Medio | Límite por visitante, solo http/https públicos, lista de bloqueo |
| El índice se llena de spam y basura | Alto (es lo que mata a los buscadores pequeños) | Fase 2 entera dedicada a esto |
| Que nadie lo visite | **El más grave de todos** | No lo resuelve el código; se planifica aparte |

---

## 9. Lo que hay que arreglar en lo que ya existe (hallazgos de hoy)

1. **`robots.txt` no se respeta** — verificado: no hay una sola línea de código que lo mire.
   Es lo primero a corregir, antes de rastrear nada nuevo.
2. **El índice es 97% Wikipedia** — medido: 969 de 995 páginas son Wikimedia. La base de
   datos `nuby_massive.db` (1.000 páginas + 7.962 entradas multimedia) quedó **rastreada
   pero sin usar**: hay trabajo hecho tirado a la basura.
3. **`data/nuby_massive.db` de la raíz está vacío** (0 filas) mientras el de `nuby_engine/`
   tiene los datos. Confunde.
4. **Los documentos se contradicen**: `ARCHITECTURE.md` dice "15/15" en varios sitios y la
   suite real da **20/20** (verificado al compilar).
5. **Restos del backend GPU** (`gpu/`, `src/gpu/`, `CMakeLists.txt`) no participan del build.
6. **La versión de navegador no puede abrir sitios ajenos** (bloqueo entre sitios del propio
   navegador). Es lo que arregla la Fase 3.
7. **Warnings de compilación** en `rasterizer.hpp` (indentación engañosa) y
   `html_preprocess.hpp` (función sin usar).

---

## 10. Decisiones que necesito de ti antes de tocar código

1. **¿Guardamos solo resúmenes cortos o también el texto completo de las páginas?**
   (Recomendación: resúmenes. Más pequeño, más legal, y el usuario acaba en el sitio real.)
2. **¿Empezamos por el buscador (Fases 0-2) o por abrir páginas (Fase 3)?**
   (Recomendación: buscador. Sin índice bueno, abrir páginas no sirve de nada.)
3. **¿Objetivo a 90 días: 100.000, 500.000 o 1.000.000 de páginas?**
   (Recomendación: 300.000. Da tiempo a limpiar calidad sin prometer de más.)
4. **¿Cuántos países/idiomas?** El motor compone acentos en español; podemos centrarnos en
   español + inglés al principio, en vez de diluir el rastreo en 50 idiomas.
