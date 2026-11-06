#!/usr/bin/env bash
# experimenta.sh — TU palanca. Metes tus numeros y te dice si PASA o NO.
#
#   tools/experimenta.sh N_FAST TAU_M_FAST TAU_M_SLOW SLOW_IN [CORPUS] [N] [SEEDS]
#
#   ej:  tools/experimenta.sh 300 6 200 0.8
#        tools/experimenta.sh 300 6 200 0.8 data/corpus.txt 2400 "1234 7 99"
#
# Lo que mide, para tu config y para BASE:
#   - lag0 / lag1 / bits condicional  (diag_mem)
#   - bits/caracter en holdout        (bench_bio, EP=10, 80/20 secuencial)
# y al final: PASA / NO PASA / MIRA.
#
# Criterio: lag1 >= 35%, lag0 >= 95%, holdout <= BASE.
# Los 4 numeros que metes son los de la poblacion lenta (la memoria):
#   N_FAST      neuronas rapidas (las que ven el presente)
#   TAU_M_FAST  membrana rapida, en ticks (cuanto vive el presente)
#   TAU_M_SLOW  membrana lenta, en ticks (cuanto vive el PASADO)  <- la memoria
#   SLOW_IN     cuanto entra al canal lento (0.8 = fuerte, 1.0 = todo)

set -u
cd "$(dirname "$0")/.."

NF=${1:?falta N_FAST}; TMF=${2:?falta TAU_M_FAST}; TMS=${3:?falta TAU_M_SLOW}
SI=${4:?falta SLOW_IN}
CORPUS=${5:-data/corpus.txt}
N=${6:-2400}
SEEDS=(${7:-1234 7 99 2024 555})

DMEM=${DMEM:-/tmp/dmem}
BBIO=${BBIO:-/tmp/bbio}

say() { printf '\n\033[1m=== %s ===\033[0m\n' "$*"; }
die() { printf '\n\033[31m[ERROR] %s\033[0m\n' "$*" >&2; exit 1; }

[ -f "$CORPUS" ] || die "no hay $CORPUS"
[ -x "$DMEM" ] && [ -x "$BBIO" ] || {
  say "compilando"; g++ -std=c++20 -O3 -march=native -Iengine engine/cortex_lm.cpp tools/diag_mem.cpp -o "$DMEM" || die diag_mem
  g++ -std=c++20 -O3 -march=native -Iengine engine/cortex_lm.cpp tools/bench_bio.cpp -o "$BBIO" || die bench_bio; }

say "Tu config: n_fast=$NF  tau_fast=$TMF  tau_slow=$TMS  slow_in=$SI"
say "1. Memoria (diag_mem)"
"$DMEM" "$CORPUS" "$N" 1234 "$NF" "$TMF" "$TMS" "$SI" || die "diag_mem falló"

say "2. Banco (bench_bio, EP=10)"
bench() { # $1=nombre  $2=cfg  $3=extra tail
  local nombre="$1" cfg="$2" extra="$3"
  for s in "${SEEDS[@]}"; do
    v=$("$BBIO" "$CORPUS" data/dic.txt 200 "$s" "$cfg" \
      0 2 0.0 1 0 0 1 $extra 1.0 2>/dev/null \
      | awk -F': ' '/holdout/{print $2}' | awk '{print $1}')
    printf '%s seed %-6s %s\n' "$nombre" "$s" "$v"
  done
}
bench "BASE    " "0,0,0,10" "0 0 0 0 0"
bench "TU CONFIG" "1,0,0,10" "$NF $TMF $TMS $SI 0"

say "3. Veredicto"
printf 'mira arriba: lag1 de TU CONFIG debe ser >= 35%% y lag0 >= 95%%\n'
printf 'y los bits de holdout de TU CONFIG deben ser <= los de BASE.\n'
