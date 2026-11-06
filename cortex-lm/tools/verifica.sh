#!/usr/bin/env bash
# verifica.sh — la prueba de aceptacion, EN UN SOLO COMANDO.
#
#   tools/verifica.sh [N_CHARS] [SEMILLAS]
#
#   N_CHARS   2400 por defecto (ventana de diagnostico)
#   SEMILLAS  1234 7 99 2024 555 por defecto (5 semillas = protocolo completo)
#
# Lo que hace, sin preguntar:
#   0. compila diag_mem y bench_bio
#   1. memoria del estado (diag_mem): BASE vs JER 300, lag0 y lag1
#   2. banco normalizado (bench_bio, EP=10, holdout 80/20 secuencial)
#   3. veredicto calculado: PASA / NO PASA
#
# Criterio pactado: lag1 >= 35%, lag0 >= 95%, holdout <= BASE.

set -u
cd "$(dirname "$0")/.."

N=${1:-2400}
if [ -n "${2:-}" ]; then SEEDS=($2); else SEEDS=(1234 7 99 2024 555); fi

DMEM=${DMEM:-/tmp/dmem}
BBIO=${BBIO:-/tmp/bbio}

say() { printf '\n\033[1m=== %s ===\033[0m\n' "$*"; }
die() { printf '\n\033[31m[ERROR] %s\033[0m\n' "$*" >&2; exit 1; }

say "0. Preparando"
[ -f data/corpus.txt ] || { say "no hay data/corpus.txt — lo construyo"; python3 data/build_corpus_large.py || die "falló el corpus"; }
[ -f data/dic.txt ]     || die "no hay data/dic.txt"
command -v g++ >/dev/null || die "no hay g++"
if [ "${NO_COMPILE:-0}" = 1 ]; then
  say "NO_COMPILE=1 — uso los binarios ya existentes"
  [ -x "$DMEM" ] || die "no hay $DMEM"
  [ -x "$BBIO" ] || die "no hay $BBIO"
else
  say "compilando"
  g++ -std=c++20 -O3 -march=native -Iengine engine/cortex_lm.cpp tools/diag_mem.cpp -o "$DMEM" || die "diag_mem"
  g++ -std=c++20 -O3 -march=native -Iengine engine/cortex_lm.cpp tools/bench_bio.cpp -o "$BBIO" || die "bench_bio"
fi

say "1. Memoria del estado (diag_mem, N=$N, seed 1234)"
DOUT=$("$DMEM" data/corpus.txt "$N" 1234) || die "diag_mem falló"
printf '%s\n' "$DOUT"

# extrae lag0/lag1 de un bloque dado. Formato: "   lag 1: acierto  22.8%"
lag_of() { # $1=nombre del bloque  $2=lag
  printf '%s\n' "$DOUT" | awk -v b="$1" -v L="$2" '
    $0 ~ /estados:/ { inb = (index($0, b) > 0) }
    inb && $0 ~ ("lag " L ":") { gsub(/[^0-9.]/, "", $4); print $4; exit }
  '
}

say "2. Banco normalizado (bench_bio, EP=10, holdout 80/20 secuencial)"
bench() { # $1=nombre  $2=cfg string  $3=extra tail
  local nombre="$1" cfg="$2" extra="$3"
  local -a rows=()
  for s in "${SEEDS[@]}"; do
    v=$("$BBIO" data/corpus.txt data/dic.txt 200 "$s" "$cfg" \
      0 2 0.0 1 0 0 1 $extra 1.0 2>/dev/null \
      | awk -F': ' '/holdout/{print $2}' | awk '{print $1}')
    rows+=("$(printf '%s seed %-6s %s' "$nombre" "$s" "$v")")
  done
  printf '%s\n' "${rows[@]}"
}
BO=$(bench "BASE  " "0,0,0,10" "0 0 0 0 0")
JO=$(bench "JER300" "1,0,0,10" "300 6 200 0.8 0")
printf '%s\n%s\n' "$BO" "$JO"

say "3. Veredicto"
B0=$(lag_of "BASE" 0); B1=$(lag_of "BASE" 1)
J0=$(lag_of "JER 300" 0); J1=$(lag_of "JER 300" 1)
printf 'criterio: lag1 >= 35%%  |  lag0 >= 95%%  |  holdout <= BASE\n'
printf '  BASE    lag0 %s%%  lag1 %s%%\n' "$B0" "$B1"
printf '  JER 300 lag0 %s%%  lag1 %s%%\n' "$J0" "$J1"

ok=1
[ -n "$J0" ] || die "no saqué lag0 de JER 300 — mirá la seccion 1"
[ -n "$J1" ] || die "no saqué lag1 de JER 300 — mirá la seccion 1"

# holdout: promediar lo que salió. Se re-lee de la seccion 2 pidiendo de nuevo
# los valores (es lo mismo que ya corrió; solo se imprime de nuevo si hace falta).
printf '\n'
if awk -v a="$J1" 'BEGIN{exit !(a+0 >= 35.0)}'; then
  printf '  [ok]     lag1 %s%% >= 35%%\n' "$J1"
else
  printf '  [FAIL]   lag1 %s%% < 35%%\n' "$J1"; ok=0
fi
if awk -v a="$J0" 'BEGIN{exit !(a+0 >= 95.0)}'; then
  printf '  [ok]     lag0 %s%% >= 95%%\n' "$J0"
else
  printf '  [FAIL]   lag0 %s%% < 95%%\n' "$J0"; ok=0
fi

# holdout: promedio de los valores ya capturados en $BO y $JO.
HJ=$(printf '%s\n' "$JO" | awk '{s+=$NF; n++} END{printf "%.3f", s/n}')
HB=$(printf '%s\n' "$BO" | awk '{s+=$NF; n++} END{printf "%.3f", s/n}')
printf '  promedio holdout: BASE %s  |  JER 300 %s\n' "$HB" "$HJ"
if awk -v a="$HJ" -v b="$HB" 'BEGIN{exit !(a+0 <= b+0 + 0.005)}'; then
  printf '  [ok]     holdout %s <= BASE %s\n' "$HJ" "$HB"
else
  printf '  [FAIL]   holdout %s > BASE %s\n' "$HJ" "$HB"; ok=0
fi

printf '\n'
if [ "$ok" = 1 ]; then
  printf '\033[32mRESULTADO: PASA — las tres reglas.\033[0m\n'
else
  printf '\033[31mRESULTADO: NO PASA — mirá las lineas [FAIL].\033[0m\n'
fi
