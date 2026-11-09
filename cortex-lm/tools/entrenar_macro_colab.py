#!/usr/bin/env python3
"""
entrenar_macro_colab.py — Fábrica Continua en Streaming por Lotes para CORTEX-LM Macro
(Google Colab / Servidor Cloud -> Checkpoint directo en Google Drive, 0 MB en tu teléfono).

Características:
  - Compila automáticamente el motor C++17 con OpenMP (`make`).
  - Soporta perfiles desde 165k hasta >1,017,856 neuronas LIF puras + 32 Macro-Columnas TRN
    + Lector Dinámico FP16 con Redondeo Estocástico (`--macro`).
  - Opera SIEMPRE en aprendizaje continuo por lotes (`alimentar ... --semantica`), indexando
    pasajes enciclopédicos en `CortezaSemanticaCSR` ("Leer = Saber"), consolidando con Sueño
    Cortical (`sonar`) y borrando cada lote temporal tras guardarlo en el `.bin`.
  - Reanudable: guarda un registro JSON junto al `.bin` en Google Drive para que, si Google
    Colab corta la sesión tras varias horas, retome exactamente en el lote siguiente.

Uso típico en Google Colab:
  python3 tools/entrenar_macro_colab.py \
      --perfil macro-526k \
      --salida /content/drive/MyDrive/CORTEX_LM_MACRO/cerebro_macro_es.bin \
      --lotes-wiki 12
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path

PERFILES = {
    "agil-82k":        ("1024,10240,16384,32768,20480", 28, 16, 4),
    "equilibrio-165k": ("1536,16384,32768,65536,49152", 28, 16, 4),
    "macro-526k":      ("2048,32768,65536,262144,163840", 32, 32, 4),
    "macro-1m":        ("2048,65536,131072,524288,294912", 32, 32, 4),
}

TEMAS_ENCICLOPEDICOS_SEMILLA = [
    "Física", "Termodinámica", "Electromagnetismo", "Mecánica_cuántica", "Relatividad_general",
    "Química", "Tabla_periódica_de_los_elementos", "Enlace_químico", "Biología_celular", "ADN",
    "Evolución_biológica", "Fotosíntesis", "Neurona", "Corteza_cerebral", "Tálamo", "Hipocampo",
    "Sinapsis", "Potencial_de_acción", "Matemáticas", "Álgebra", "Cálculo_infinitesimal",
    "Teoría_de_la_probabilidad", "Lógica_matemática", "Algoritmo", "Estructura_de_datos",
    "Sistema_operativo", "Arquitectura_de_computadoras", "Red_de_computadoras", "Criptografía",
    "Astronomía", "Sistema_solar", "Agujero_negro", "Geología", "Ciclo_del_agua", "Ecología",
    "Historia_universal", "Filosofía_de_la_ciencia", "Método_científico", "Ingeniería_eléctrica",
    "Superconductividad", "Óptica", "Acústica", "Medicina", "Sistema_inmunitario", "Genética",
    "Economía", "Lingüística", "Gramática_del_español", "Literatura_en_español", "Robótica",
]

CONOCIMIENTO_FUNDACIONAL_MULTIDOMINIO = """
La física estudia la materia, la energía, el espacio y el tiempo mediante leyes verificables experimentalmente.
La primera ley de la termodinámica establece que la energía total de un sistema aislado se conserva y solo se transforma.
La segunda ley de la termodinámica indica que la entropía total de un sistema aislado tiende a aumentar con el tiempo.

Las neuronas biológicas transmiten información mediante potenciales de acción o espigas eléctricas todo-o-nada a través de sus axones.
El tálamo actúa como estación de relevo y filtrado cortical, seleccionando qué macro-columnas reciben ganancia excitatoria en cada instante.
El hipocampo consolida recuerdos episódicos rápidos en la región CA3 y los transfiere gradualmente a la neocorteza durante el sueño de ondas lentas.

Un algoritmo de búsqueda binaria divide repetidamente a la mitad un arreglo ordenado, logrando una complejidad logarítmica O(log n).
Las matrices dispersas en formato CSR almacenan únicamente las conexiones no nulas, permitiendo escalar millones de sinapsis con memoria acotada.
En lógica deductiva, si toda premisa verdadera implica una consecuencia necesaria, la combinación de dos premisas independientes permite derivar conclusiones nuevas.

El ADN almacena las instrucciones genéticas de los organismos vivos mediante secuencias de cuatro bases nitrogenadas: adenina, timina, citosina y guanina.
La fotosíntesis convierte energía luminosa, agua y dióxido de carbono en glucosa y oxígeno molecular dentro de los cloroplastos vegetales.
""".strip()


def run_cmd(cmd, cwd):
    print(f"\n$ {' '.join(cmd)}", flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def cargar_registro(ruta_reg: Path) -> dict:
    if ruta_reg.exists():
        try:
            return json.loads(ruta_reg.read_text(encoding="utf-8"))
        except Exception:
            pass
    return {"fases_completadas": [], "articulos_wiki": []}


def guardar_registro(ruta_reg: Path, reg: dict):
    ruta_reg.parent.mkdir(parents=True, exist_ok=True)
    ruta_reg.write_text(json.dumps(reg, indent=2, ensure_ascii=False), encoding="utf-8")


def descargar_resumen_wiki_es(titulo: str, timeout: int = 12) -> str:
    url = (
        "https://es.wikipedia.org/api/rest_v1/page/summary/"
        + urllib.parse.quote(titulo, safe="")
    )
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "CORTEX-LM-Macro-Trainer/1.0 (Academic Spiking Research)"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        datos = json.loads(r.read().decode("utf-8", errors="replace"))
        ext = str(datos.get("extract", "")).strip()
        return ext if len(ext) >= 80 else ""


def iterar_lotes_wikipedia_streaming(ya_vistos: set, num_lotes: int, bytes_por_lote: int = 240_000):
    """
    Intenta primero streaming de HuggingFace `wikimedia/wikipedia` (20231101.es) sin descargar
    el dataset a disco; si `datasets` no está instalado, usa la API pública REST de Wikipedia ES.
    """
    try:
        from datasets import load_dataset  # type: ignore

        print("[STREAMING] Conectando flujo continuo wikimedia/wikipedia (20231101.es)...", flush=True)
        ds = load_dataset("wikimedia/wikipedia", "20231101.es", split="train", streaming=True)
        buf = []
        buf_len = 0
        emitidos = 0
        for item in ds:
            titulo = str(item.get("title", "")).strip()
            if not titulo or titulo in ya_vistos:
                continue
            texto = str(item.get("text", "")).strip()
            if len(texto) < 350:
                continue
            # Tomar los primeros párrafos sustantivos del artículo para máxima diversidad temática
            parrafos = [p.strip() for p in texto.split("\n\n") if len(p.strip()) >= 70][:6]
            if not parrafos:
                continue
            bloque = "\n\n".join(parrafos)
            ya_vistos.add(titulo)
            buf.append(bloque)
            buf_len += len(bloque)
            if buf_len >= bytes_por_lote:
                emitidos += 1
                yield f"wiki_hf_lote_{emitidos:04d}", "\n\n".join(buf)
                buf, buf_len = [], 0
                if emitidos >= num_lotes:
                    return
        if buf and emitidos < num_lotes:
            emitidos += 1
            yield f"wiki_hf_lote_{emitidos:04d}", "\n\n".join(buf)
        return
    except Exception as exc:
        print(f"[AVISO] Streaming HuggingFace no disponible ({exc}); usando API REST Wikipedia ES.", flush=True)

    buf = []
    buf_len = 0
    emitidos = 0
    for titulo in TEMAS_ENCICLOPEDICOS_SEMILLA:
        if titulo in ya_vistos:
            continue
        try:
            ext = descargar_resumen_wiki_es(titulo)
            if ext:
                ya_vistos.add(titulo)
                buf.append(ext)
                buf_len += len(ext)
                if buf_len >= min(bytes_por_lote, 28_000):
                    emitidos += 1
                    yield f"wiki_api_lote_{emitidos:04d}", "\n\n".join(buf)
                    buf, buf_len = [], 0
                    if emitidos >= num_lotes:
                        return
        except Exception:
            continue
    # Respaldo adicional inagotable por resúmenes aleatorios de Wikipedia ES si aún faltan lotes
    intentos_rand = 0
    while emitidos < num_lotes and intentos_rand < num_lotes * 40:
        intentos_rand += 1
        try:
            req = urllib.request.Request(
                "https://es.wikipedia.org/api/rest_v1/page/random/summary",
                headers={"User-Agent": "CORTEX-LM-Macro-Trainer/1.0 (Academic Spiking Research)"},
            )
            with urllib.request.urlopen(req, timeout=12) as r:
                datos = json.loads(r.read().decode("utf-8", errors="replace"))
                tit = str(datos.get("title", "")).strip()
                ext = str(datos.get("extract", "")).strip()
                if tit and tit not in ya_vistos and len(ext) >= 120:
                    ya_vistos.add(tit)
                    buf.append(ext)
                    buf_len += len(ext)
                    if buf_len >= min(bytes_por_lote, 60_000):
                        emitidos += 1
                        yield f"wiki_rand_lote_{emitidos:04d}", "\n\n".join(buf)
                        buf, buf_len = [], 0
        except Exception:
            continue
    if buf and emitidos < num_lotes:
        emitidos += 1
        yield f"wiki_api_lote_{emitidos:04d}", "\n\n".join(buf)


def main():
    ap = argparse.ArgumentParser(description="Entrenador Macro en Streaming Continuo para Google Colab")
    ap.add_argument("--perfil", choices=list(PERFILES.keys()), default="macro-526k")
    ap.add_argument("--capas", default="", help="Sobrescribir CSV de capas manualmente")
    ap.add_argument("--salida", required=True, help="Ruta del .bin (ej. en /content/drive/MyDrive/...)")
    ap.add_argument("--lotes-libros", type=int, default=4, help="Cuántos lotes literarios GITenberg alimentar")
    ap.add_argument("--lotes-wiki", type=int, default=8, help="Cuántos lotes enciclopédicos en streaming alimentar")
    ap.add_argument("--bytes-lote", type=int, default=260_000, help="Tamaño por lote temporal en bytes")
    args = ap.parse_args()

    raiz = Path(__file__).resolve().parent.parent
    bin_cerebro = raiz / "bin" / "cerebro"
    ruta_modelo = Path(args.salida).resolve()
    ruta_modelo.parent.mkdir(parents=True, exist_ok=True)
    ruta_reg = ruta_modelo.with_suffix(".registro.json")
    reg = cargar_registro(ruta_reg)

    # 1. Compilar con OpenMP si hace falta
    if not bin_cerebro.exists():
        run_cmd(["make", f"-j{os.cpu_count() or 2}"], cwd=str(raiz))

    # 2. Preparar paquetes base ligeros (alineación deliberativa y base multidominio)
    run_cmd([sys.executable, "tools/generar_alineacion.py"], cwd=str(raiz))
    if not (raiz / "data" / "corpus.txt").exists():
        run_cmd([sys.executable, "build_corpus.py"], cwd=str(raiz / "data"))

    ruta_ali = str(raiz / "data" / "alineacion_es.txt")
    ruta_con = raiz / "data" / "conocimiento_es.txt"
    texto_arranque = ruta_con.read_text(encoding="utf-8") + "\n\n" + CONOCIMIENTO_FUNDACIONAL_MULTIDOMINIO + "\n\n" + (raiz / "data" / "corpus.txt").read_text(encoding="utf-8")

    capas_csv, grado, ncols, topk = PERFILES[args.perfil]
    if args.capas:
        capas_csv = args.capas

    # 3. Creación inicial del cerebro Macro (solo si aún no existe en Drive)
    if not ruta_modelo.exists():
        with tempfile.NamedTemporaryFile("w", suffix=".txt", encoding="utf-8", delete=False) as tf:
            tf.write(texto_arranque)
            tmp_init = tf.name
        try:
            run_cmd(
                [
                    str(bin_cerebro), "entrenar", str(ruta_modelo), tmp_init,
                    "--capas", capas_csv,
                    "--grado", str(grado),
                    "--columnas", str(ncols),
                    "--top-columnas", str(topk),
                    "--macro",
                    "--epocas", "3",
                    "--lr", "0.45",
                    "--stdp", "1",
                    "--holdout", "0.05",
                ],
                cwd=str(raiz),
            )
            run_cmd(
                [
                    str(bin_cerebro), "alinear", str(ruta_modelo), ruta_ali,
                    "--epocas", "22", "--lr", "0.40", "--ancla", "0.01",
                ],
                cwd=str(raiz),
            )
            run_cmd(
                [
                    str(bin_cerebro), "asimilar", str(ruta_modelo), tmp_init,
                    "--max-pasajes", "50", "--ciclos", "18",
                ],
                cwd=str(raiz),
            )
            reg["fases_completadas"].append("arranque_macro")
            guardar_registro(ruta_reg, reg)
        finally:
            if os.path.exists(tmp_init):
                os.remove(tmp_init)

    # 4. Alimentación continua de clásicos públicos en español (descarga temporal y borrado inmediato)
    if args.lotes_libros > 0 and "libros_gitenberg" not in reg["fases_completadas"]:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_corpus = os.path.join(tmpdir, "clasicos_es.txt")
            try:
                run_cmd(
                    [sys.executable, "tools/descargar_corpus.py", "--salida", tmp_corpus, "--max-mb", "2.0"],
                    cwd=str(raiz),
                )
                texto_lib = Path(tmp_corpus).read_text(encoding="utf-8", errors="ignore")
                os.remove(tmp_corpus)
                tramos = [
                    texto_lib[i : i + args.bytes_lote]
                    for i in range(0, len(texto_lib), args.bytes_lote)
                ][: args.lotes_libros]
                for idx, tramo in enumerate(tramos, 1):
                    id_fase = f"libro_lote_{idx:03d}"
                    if id_fase in reg["fases_completadas"]:
                        continue
                    lote_path = os.path.join(tmpdir, f"{id_fase}.txt")
                    Path(lote_path).write_text(tramo, encoding="utf-8")
                    run_cmd(
                        [
                            str(bin_cerebro), "alimentar", str(ruta_modelo), lote_path,
                            "--bloque", str(args.bytes_lote), "--epocas", "1",
                            "--lr", "0.24", "--ancla", "0.022",
                        ],
                        cwd=str(raiz),
                    )
                    os.remove(lote_path)
                    reg["fases_completadas"].append(id_fase)
                    guardar_registro(ruta_reg, reg)
                reg["fases_completadas"].append("libros_gitenberg")
                guardar_registro(ruta_reg, reg)
            except Exception as exc:
                print(f"[AVISO] No se pudieron descargar libros externos ({exc}); continuando.", flush=True)

    # 5. Streaming enciclopédico multidominio (Wikipedia ES) con Leer=Saber CSR + borrado de lotes
    if args.lotes_wiki > 0:
        vistos = set(reg.get("articulos_wiki", []))
        for nombre_lote, contenido in iterar_lotes_wikipedia_streaming(vistos, args.lotes_wiki, args.bytes_lote):
            if nombre_lote in reg["fases_completadas"]:
                continue
            with tempfile.NamedTemporaryFile("w", suffix=".txt", encoding="utf-8", delete=False) as tf:
                tf.write(contenido)
                tmp_lote = tf.name
            try:
                t0 = time.time()
                run_cmd(
                    [
                        str(bin_cerebro), "alimentar", str(ruta_modelo), tmp_lote,
                        "--bloque", str(args.bytes_lote), "--epocas", "1",
                        "--lr", "0.24", "--ancla", "0.022",
                        "--semantica", "--max-pasajes", "20",
                    ],
                    cwd=str(raiz),
                )
                print(f"[STREAMING OK] {nombre_lote} consolidado y lote temporal eliminado ({time.time()-t0:.1f}s).")
                reg["fases_completadas"].append(nombre_lote)
                reg["articulos_wiki"] = sorted(list(vistos))[-5000:]
                guardar_registro(ruta_reg, reg)
            finally:
                if os.path.exists(tmp_lote):
                    os.remove(tmp_lote)

    # 6. Consolidación final por Sueño Cortical de Ondas Lentas
    run_cmd([str(bin_cerebro), "sonar", str(ruta_modelo), "--ciclos", "18"], cwd=str(raiz))
    tam_mb = ruta_modelo.stat().st_size / (1024 * 1024)
    print(f"\n=== CEREBRO MACRO VIVO LISTO EN '{ruta_modelo}' ({tam_mb:.1f} MB) ===", flush=True)


if __name__ == "__main__":
    main()
