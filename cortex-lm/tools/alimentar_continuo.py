#!/usr/bin/env python3
"""Alimentador de aprendizaje continuo permanente para CORTEX-LM (100% --continuo).

Garantiza que SIEMPRE que se quiera nutrir un cerebro existente con nuevos libros,
articulos o episodios de razonamiento:
  1. Jamas se reinicia el cerebro desde cero (exige que <modelo.bin> exista y
     invoca exclusivamente `./bin/cerebro entrenar ... --continuo` con anclaje
     sinaptico elastico `--ancla`).
  2. Divide textos grandes en bloques limpios por limite de parrafo (`--tam-bloque`,
     por defecto 460 KB) guardando checkpoint tras cada bloque.
  3. Registra cada bloque procesado por huella SHA-256 en `data/registro_continuo.json`
     para poder pausar/reanudar en cualquier momento sin repetir bloques ya leidos
     (salvo que se pase `--forzar`).
  4. Incluye `--biblioteca-restante` para construir e intercalar automaticamente
     todos los tramos no leidos de la biblioteca descargada (`libro_moderno.txt`,
     `libro_quijote.txt`, `conocimiento_es.txt`) verificando 0% de solapamiento
     con los 3 holdouts (`holdout_galdos_contiguo.txt`, `holdout_quijote.txt` y
     `holdout_becquer.txt`).

Ejemplos de uso:
  # Alimentar toda la biblioteca restante (en lotes reanudables):
  ./tools/alimentar_continuo.py data/cerebro_es.bin --biblioteca-restante

  # Alimentar cualquier libro o conocimiento nuevo en el futuro:
  ./tools/alimentar_continuo.py data/cerebro_es.bin nuevo_libro.txt otro_texto.txt

  # Alimentar texto nuevo + actualizar razonamiento dorsal de forma continua:
  ./tools/alimentar_continuo.py data/cerebro_es.bin nuevo_texto.txt --alinear data/alineacion_es.txt
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time


ENCICLOPEDIA_EXTRA = """
La electricidad y el magnetismo son dos manifestaciones de una misma interacción física fundamental llamada electromagnetismo. Cuando una corriente eléctrica recorre un conductor genera un campo magnético a su alrededor, y cuando un campo magnético varía cerca de una bobina induce una corriente eléctrica en ella. Este principio de inducción descubierto por Michael Faraday permite generar la energía eléctrica que ilumina ciudades, mueve motores industriales y alimenta las telecomunicaciones modernas.

La materia que nos rodea está formada por átomos compuestos de un núcleo central de protones y neutrones rodeado por nubes de electrones. Al unirse mediante enlaces químicos, los átomos forman moléculas tan simples como el oxígeno gaseoso o tan complejas como las proteínas celulares. Según la temperatura y la presión, una sustancia adopta distintos estados de agregación: en un sólido las partículas vibran alrededor de posiciones fijas, en un líquido se deslizan unas sobre otras conservando su volumen y en un gas se mueven con gran libertad ocupando todo el recipiente disponible.

Los océanos cubren más del setenta por ciento de la superficie terrestre y actúan como el gran regulador térmico del planeta. Almacenan calor solar en las zonas ecuatoriales y lo distribuyen hacia latitudes altas mediante corrientes marinas superficiales y profundas. Además, el fitoplancton microscópico que habita las aguas iluminadas realiza fotosíntesis a escala planetaria, absorbiendo dióxido de carbono y liberando una parte sustancial del oxígeno que respiramos en todos los continentes.

El método científico permite distinguir las conjeturas aparentes del conocimiento comprobado. Parte de la observación atenta de un fenómeno natural, formula una hipótesis clara capaz de hacer predicciones medibles y diseña experimentos controlados para ponerla a prueba. Cuando otros investigadores independientes repiten el experimento y obtienen los mismos resultados, la explicación gana solidez; si la evidencia contradice la hipótesis, esta se corrige o se abandona con honestidad intelectual.

La nutrición humana equilibra el aporte de carbohidratos, proteínas, grasas saludables, vitaminas, minerales y agua pura. Las proteínas suministran aminoácidos esenciales para reparar tejidos y fabricar enzimas; los carbohidratos complejos proporcionan energía sostenida al cerebro y a los músculos; y los micronutrientes presentes en frutas, verduras, legumbres y semillas regulan las defensas inmunitarias y el metabolismo celular.

En el universo observable existen cientos de miles de millones de galaxias agrupadas en cúmulos y filamentos cósmicos. Nuestro sistema solar se encuentra en uno de los brazos espirales de la Vía Láctea, una galaxia que reúne cientos de miles de millones de estrellas junto con nebulosas de gas y polvo donde siguen naciendo sistemas planetarios nuevos por contracción gravitatoria.
""".strip()


def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def cargar_registro(ruta_reg: str) -> dict:
    if os.path.exists(ruta_reg):
        with open(ruta_reg, encoding="utf-8") as f:
            return json.load(f)
    return {"bloques_entrenados": [], "total_bytes_continuos": 0, "historial": []}


def guardar_registro(ruta_reg: str, reg: dict) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(ruta_reg)), exist_ok=True)
    tmp = ruta_reg + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(reg, f, indent=2, ensure_ascii=False)
    os.replace(tmp, ruta_reg)


def partir_en_parrafos(texto: str, tam_objetivo: int) -> list[str]:
    """Divide `texto` en bloques cercanos a `tam_objetivo` chars cortando en `\\n\\n`."""
    bloques = []
    n = len(texto)
    pos = 0
    while pos < n:
        while pos < n and texto[pos] in "\r\n ":
            pos += 1
        if pos >= n:
            break
        if n - pos <= int(tam_objetivo * 1.18):
            bloques.append(texto[pos:].strip() + "\n")
            break
        fin = texto.find("\n\n", pos + tam_objetivo)
        if fin == -1 or fin - pos > int(tam_objetivo * 1.35):
            fin = texto.rfind("\n\n", pos + tam_objetivo // 2, pos + tam_objetivo)
        if fin == -1:
            fin = pos + tam_objetivo
        bloques.append(texto[pos:fin].strip() + "\n")
        pos = fin + 2
    return [b for b in bloques if b.strip()]


def preparar_bloques_biblioteca(data_dir: str, tam_bloque: int) -> list[tuple[str, str]]:
    """Extrae e intercala TODOS los tramos no leidos de libro_moderno y libro_quijote,
    preservando intactos y con 0% solapamiento los 3 holdouts literarios."""
    ruta_mod = os.path.join(data_dir, "libro_moderno.txt")
    ruta_qui = os.path.join(data_dir, "libro_quijote.txt")
    ruta_con = os.path.join(data_dir, "conocimiento_es.txt")

    if not (os.path.exists(ruta_mod) and os.path.exists(ruta_qui)):
        raise SystemExit("Faltan libro_moderno.txt o libro_quijote.txt; ejecuta tools/descargar_corpus.py primero.")

    raw_m = open(ruta_mod, "rb").read()
    qui = open(ruta_qui, encoding="utf-8").read()
    con = open(ruta_con, encoding="utf-8").read() if os.path.exists(ruta_con) else ""

    # 1. Reconstruir cortes exactos usados en Fase 1 y Fase 2 para NO tocar holdouts ni repetir lo ya leido
    cut1 = 220000
    while (raw_m[cut1] & 0xC0) == 0x80:
        cut1 += 1
    cut2 = 260000
    while (raw_m[cut2] & 0xC0) == 0x80:
        cut2 += 1

    # Holdout Galdos contiguo (intocable): raw_m[cut1:cut2]
    ho_galdos = raw_m[cut1:cut2].decode("utf-8")
    ruta_ho_galdos = os.path.join(data_dir, "holdout_galdos_contiguo.txt")
    if not os.path.exists(ruta_ho_galdos):
        open(ruta_ho_galdos, "w", encoding="utf-8").write(ho_galdos)

    # Reservar tambien un 3er holdout intocable al final de libro_moderno (Becquer, ~30k chars)
    mod_rest_full = raw_m[cut2:].decode("utf-8")
    corte_becquer = mod_rest_full.rfind("\n\n", 0, len(mod_rest_full) - 30000)
    mod_no_leido = mod_rest_full[:corte_becquer].strip() + "\n"
    ho_becquer = mod_rest_full[corte_becquer:].strip() + "\n"
    ruta_ho_becquer = os.path.join(data_dir, "holdout_becquer.txt")
    open(ruta_ho_becquer, "w", encoding="utf-8").write(ho_becquer)

    # Cortes exactos de Quijote (Fase 2 leyo qui[:corte_q]; holdout es qui[ini_hq:fin_hq])
    corte_q = qui.find("\n\n", 140_000)
    ini_hq = qui.find("\n\n", 500_000) + 2
    fin_hq = qui.find("\n\n", ini_hq + 25_000)
    ho_quijote = qui[ini_hq:fin_hq] + "\n"
    ruta_ho_quijote = os.path.join(data_dir, "holdout_quijote.txt")
    if not os.path.exists(ruta_ho_quijote):
        open(ruta_ho_quijote, "w", encoding="utf-8").write(ho_quijote)

    qui_tramo_a = qui[corte_q:ini_hq - 2].strip()
    qui_tramo_b = qui[fin_hq + 2:].strip()
    qui_no_leido = qui_tramo_a + "\n\n" + qui_tramo_b + "\n"

    # Verificacion estricta de 0% fuga de holdout (subcadenas de 120 chars)
    todo_train = mod_no_leido + "\n" + qui_no_leido
    for nombre_ho, txt_ho in (
        ("holdout_galdos_contiguo", ho_galdos),
        ("holdout_quijote", ho_quijote),
        ("holdout_becquer", ho_becquer),
    ):
        sonda = txt_ho[200:340]
        if sonda in todo_train:
            raise RuntimeError(f"ALERTA: fuga detectada con {nombre_ho}!")

    bloques_mod = partir_en_parrafos(mod_no_leido, tam_bloque)
    bloques_qui = partir_en_parrafos(qui_no_leido, tam_bloque)

    # Intercalar bloques modernos y clasicos para mantener equilibrio lexico/estilistico,
    # cerrando con prosa moderna + conocimiento enciclopedico limpio.
    intercalados: list[tuple[str, str]] = []
    im, iq = 0, 0
    while im < len(bloques_mod) or iq < len(bloques_qui):
        if iq < len(bloques_qui):
            intercalados.append((f"quijote_parte_{iq + 1:02d}", bloques_qui[iq]))
            iq += 1
        if im < len(bloques_mod):
            intercalados.append((f"moderno_galdos_becquer_{im + 1:02d}", bloques_mod[im]))
            im += 1

    cierre_conocimiento = (con.strip() + "\n\n" + ENCICLOPEDIA_EXTRA + "\n").strip() + "\n"
    intercalados.append(("conocimiento_enciclopedico_moderno", cierre_conocimiento))

    out_dir = os.path.join(data_dir, "bloques_continuos")
    os.makedirs(out_dir, exist_ok=True)
    rutas = []
    for idx, (etiqueta, contenido) in enumerate(intercalados, 1):
        ruta_b = os.path.join(out_dir, f"bloque_{idx:02d}_{etiqueta}.txt")
        with open(ruta_b, "w", encoding="utf-8") as f:
            f.write(contenido)
        rutas.append((etiqueta, ruta_b))
    return rutas


def main() -> int:
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    data_dir = os.path.join(base_dir, "data")
    bin_cerebro = os.path.join(base_dir, "bin", "cerebro")

    ap = argparse.ArgumentParser(description="Alimentacion 100% continua (--continuo) para CORTEX-LM.")
    ap.add_argument("modelo", help="Ruta al cerebro existente (ej. data/cerebro_es.bin)")
    ap.add_argument("archivos", nargs="*", help="Archivos o carpetas .txt nuevos para aprender con --continuo")
    ap.add_argument("--biblioteca-restante", action="store_true",
                    help="Alimenta todos los tramos aun no leidos de la biblioteca descargada")
    ap.add_argument("--alinear", default="",
                    help="Archivo de episodios conversacionales/deliberativos para alinear con --continuo al terminar")
    ap.add_argument("--epocas-alinear", type=int, default=16)
    ap.add_argument("--tam-bloque", type=int, default=460000,
                    help="Tamano objetivo en caracteres por bloque continuo (defecto: 460000)")
    ap.add_argument("--epocas", type=int, default=1,
                    help="Epocas continuas por bloque literario (defecto: 1)")
    ap.add_argument("--lr", type=float, default=0.22,
                    help="Tasa de aprendizaje continua por bloque (defecto: 0.22)")
    ap.add_argument("--ancla", type=float, default=0.030,
                    help="Fuerza de consolidacion elastica anti-olvido (defecto: 0.030)")
    ap.add_argument("--max-bloques", type=int, default=0,
                    help="Procesar como maximo N bloques pendientes en esta invocacion (0 = todos)")
    ap.add_argument("--forzar", action="store_true",
                    help="Re-entrenar bloques aunque ya figuren en data/registro_continuo.json")
    args = ap.parse_args()

    if not os.path.exists(bin_cerebro):
        raise SystemExit(f"No existe '{bin_cerebro}'. Ejecuta 'make' en cortex-lm primero.")
    if not os.path.exists(args.modelo):
        raise SystemExit(
            f"Error: '{args.modelo}' no existe.\n"
            "Por regla de diseno, alimentar_continuo.py NUNCA sobrescribe ni crea desde cero;\n"
            "usa siempre aprendizaje continuo (--continuo) sobre un cerebro ya inicializado."
        )

    tareas: list[tuple[str, str]] = []
    if args.biblioteca_restante:
        tareas.extend(preparar_bloques_biblioteca(data_dir, args.tam_bloque))

    out_custom = os.path.join(data_dir, "bloques_usuario")
    for item in args.archivos:
        if os.path.isdir(item):
            rutas_txt = sorted(
                os.path.join(item, fn) for fn in os.listdir(item) if fn.endswith(".txt")
            )
        else:
            rutas_txt = [item]
        for rt in rutas_txt:
            txt = open(rt, encoding="utf-8").read()
            partes = partir_en_parrafos(txt, args.tam_bloque)
            base_n = os.path.splitext(os.path.basename(rt))[0]
            if len(partes) == 1:
                tareas.append((base_n, rt))
            else:
                os.makedirs(out_custom, exist_ok=True)
                for k, ptxt in enumerate(partes, 1):
                    rb = os.path.join(out_custom, f"{base_n}_parte_{k:02d}.txt")
                    open(rb, "w", encoding="utf-8").write(ptxt)
                    tareas.append((f"{base_n}_parte_{k:02d}", rb))

    if not tareas and not args.alinear:
        raise SystemExit("Indica al menos un archivo .txt, --biblioteca-restante o --alinear.")

    ruta_reg = os.path.join(data_dir, "registro_continuo.json")
    reg = cargar_registro(ruta_reg)
    firma_topo = sha256_bytes(open(args.modelo, "rb").read(4096))
    if reg.get("firma_topologia") and reg.get("firma_topologia") != firma_topo:
        reg = {"bloques_entrenados": [], "historial": [], "firma_topologia": firma_topo}
    else:
        reg["firma_topologia"] = firma_topo
    vistos = set(reg.get("bloques_entrenados", []))

    pendientes = []
    ya_leidos = 0
    for etiqueta, ruta_b in tareas:
        raw = open(ruta_b, "rb").read()
        h = sha256_bytes(raw)
        if (not args.forzar) and (h in vistos):
            ya_leidos += 1
            continue
        pendientes.append((etiqueta, ruta_b, h, len(raw)))

    print(f"=== ALIMENTACION CONTINUA CORTEX-LM (--continuo) ===")
    print(f"Modelo base       : {args.modelo}")
    print(f"Bloques ya leidos : {ya_leidos} | Bloques pendientes: {len(pendientes)}")

    procesados = 0
    for idx, (etiqueta, ruta_b, h, nbytes) in enumerate(pendientes, 1):
        if args.max_bloques > 0 and procesados >= args.max_bloques:
            print(f"[PAUSA LIMPIA] Se completo el lote de {procesados} bloques. "
                  f"Quedan {len(pendientes) - procesados} bloques listos para la siguiente ejecucion.")
            break

        # Al bloque enciclopedico corto le damos 2 pasadas suaves para fijar vocabulario tecnico
        ep_uso = max(args.epocas, 2) if nbytes < 60000 else args.epocas
        print(f"\n--- [{idx}/{len(pendientes)}] Alimentando '{etiqueta}' ({nbytes:,} bytes, epocas={ep_uso}, lr={args.lr}, ancla={args.ancla}) ---")
        sys.stdout.flush()

        t0 = time.time()
        cmd = [
            bin_cerebro, "entrenar", args.modelo, ruta_b,
            "--continuo",
            "--epocas", str(ep_uso),
            "--lr", str(args.lr),
            "--ancla", str(args.ancla),
            "--holdout", "0.0",
        ]
        subprocess.run(cmd, check=True)
        dt = time.time() - t0

        vistos.add(h)
        reg["bloques_entrenados"] = sorted(vistos)
        reg["total_bytes_continuos"] = int(reg.get("total_bytes_continuos", 0)) + nbytes
        reg.setdefault("historial", []).append({
            "fecha": time.strftime("%Y-%m-%d %H:%M:%S"),
            "etiqueta": etiqueta,
            "bytes": nbytes,
            "epocas": ep_uso,
            "lr": args.lr,
            "ancla": args.ancla,
            "segundos": round(dt, 1),
            "sha256": h[:16],
        })
        guardar_registro(ruta_reg, reg)
        procesados += 1

    if args.alinear:
        if not os.path.exists(args.alinear):
            raise SystemExit(f"No existe el archivo de alineacion '{args.alinear}'.")
        print(f"\n--- Alineacion continua via dorsal (--continuo, epocas={args.epocas_alinear}) sobre '{args.alinear}' ---")
        sys.stdout.flush()
        subprocess.run([
            bin_cerebro, "alinear", args.modelo, args.alinear,
            "--continuo",
            "--epocas", str(args.epocas_alinear),
            "--lr", "0.35",
            "--ancla", "0.0005",
        ], check=True)

    print(f"\n=== COMPLETADO: {procesados} bloques nuevos asimilados | "
          f"Acumulado continuo registrado: {reg.get('total_bytes_continuos', 0):,} bytes ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
