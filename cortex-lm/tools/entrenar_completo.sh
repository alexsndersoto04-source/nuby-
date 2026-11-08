#!/usr/bin/env bash
# entrenar_completo.sh — Pipeline reproducible de CORTEX-LM (100% spiking LIF puro):
#   1. Compila motor C++ y verifica auditoria estructural (5/5 TODO OK)
#   2. Descarga corpus literario en espanol limpio (Galdos, Becquer, Cervantes)
#   3. Entrena Fase 1 (literatura moderna), Fase 2 (--continuo: Cervantes + expositivo)
#      y Fase 3 (alineacion conversacional dorsal)
#   4. Evalua BPC en holdout jamas visto y muestra respuestas en vivo

set -euo pipefail
cd "$(dirname "$0")/.."

echo "=== [1/4] Compilacion y auditoria estructural ==="
make
./bin/cortex_lm audit

echo "=== [2/4] Preparacion de corpus amplio y episodios de alineacion ==="
python3 data/build_corpus_large.py
python3 tools/descargar_corpus.py
python3 tools/generar_alineacion.py

python3 - <<'PY'
mod = open("data/libro_moderno.txt", encoding="utf-8").read()
qui = open("data/libro_quijote.txt", encoding="utf-8").read()
con = open("data/conocimiento_es.txt", encoding="utf-8").read()

raw_m = mod.encode("utf-8")
cut1 = 220000
while (raw_m[cut1] & 0xC0) == 0x80: cut1 += 1
cut2 = 260000
while (raw_m[cut2] & 0xC0) == 0x80: cut2 += 1
open("data/train_moderno.txt", "wb").write(raw_m[:cut1])
open("data/holdout_galdos_contiguo.txt", "wb").write(raw_m[cut1:cut2])

corte_q = qui.find("\n\n", 140_000)
open("data/train_continuo.txt", "w", encoding="utf-8").write(qui[:corte_q] + "\n\n" + con + "\n")

ini_hq = qui.find("\n\n", 500_000) + 2
fin_hq = qui.find("\n\n", ini_hq + 25_000)
open("data/holdout_quijote.txt", "w", encoding="utf-8").write(qui[ini_hq:fin_hq] + "\n")
PY

echo "=== [3/4] Entrenamiento Fase 1 + Aprendizaje Continuo Fase 2 + Alineacion Deliberativa Fase 3 ==="
./bin/cerebro entrenar data/cerebro_es.bin data/train_moderno.txt \
    --capas 1024,10240,10240,8192 --grado 48 --epocas 3 --lr 0.45 --stdp 1 --holdout 0.0 --seed 1234

./bin/cerebro entrenar data/cerebro_es.bin data/train_continuo.txt \
    --continuo --epocas 3 --lr 0.30 --ancla 0.025 --holdout 0.0

./bin/cerebro alinear data/cerebro_es.bin data/alineacion_es.txt \
    --epocas 28 --lr 0.50 --ancla 0.001

echo "=== [4/4] Verificacion sobre holdouts no vistos, razonamiento y dialogo multi-turno ==="
./bin/cerebro evaluar data/cerebro_es.bin data/holdout_galdos_contiguo.txt data/dic_amplio.txt
./bin/cerebro evaluar data/cerebro_es.bin data/holdout_quijote.txt data/dic_amplio.txt
./bin/cerebro responder data/cerebro_es.bin "¿Por qué un cubo de hielo flota en el agua?" --pensar --temp 0.08
./bin/cerebro responder data/cerebro_es.bin "¿Y cómo se llama el escudero en esa novela?" \
    --prev "¿Quién escribió el Quijote?" --pensar --temp 0.08
echo ""
echo "Listo. Para alimentar el resto de la biblioteca o nuevos libros SIEMPRE con --continuo:"
echo "  ./tools/alimentar_continuo.py data/cerebro_es.bin --biblioteca-restante"
echo "  ./tools/alimentar_continuo.py data/cerebro_es.bin ruta/a/nuevo_libro.txt"
echo "Y para conversar en vivo (con deliberacion y memoria CA3):"
echo "  ./bin/cerebro interactivo data/cerebro_es.bin --pensar"
