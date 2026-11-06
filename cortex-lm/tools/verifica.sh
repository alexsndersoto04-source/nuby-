#!/usr/bin/env bash
# verifica.sh — la prueba de aceptacion, EN UN SOLO COMANDO.
#
#   tools/verifica.sh [N_CHARS] [SEMILLAS]
#
#   N_CHARS   2400 por defecto (ventana de diagnostico)
#   SEMILLAS  1234 7 99 2024 555 por defecto (5 semillas = protocolo completo)
#
# Lo que hace, sin preguntar:
#   0. compila diag_mem, bench_bio
#   1. memoria del estado (diag_mem): BASE vs JER 300, lag0 y lag1
#   2. banco normalizado (bench_bio, EP=10, holdout 80/20 secuencial),
#      tres configs: BASE, BASE con 2 estados pasados, y JER 300
#   3. veredicto calculado
#
# Criterio pactado (memoria): lag1 >= 35%, lag0 >= 95%, holdout <= BASE.
# Criterio funcional (reciente): el lector con 2 estados pasados debe estar
#   <= BASE en holdout. Esa es la mejora que se encontro.

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

lag_of() { # $1=nombre del bloque  $2=lag
  printf '%s\n' "$DOUT" | awk -v b="$1" -v L="$2" '
    $0 ~ /estados:/ { inb = (index($0, b) > 0) }
    inb && $0 ~ ("lag " L ":") { gsub(/[^0-9.]/, "", $4); print $4; exit }
  '
}

say "2. Banco normalizado (bench_bio, EP=10, holdout 80/20 secuencial)"
# Cada config: "JER,DOP,OSC,EP" + args. El order esta en tools/bench_bio.cpp:
#   mostrar g_feat g_pace g_taps g_wscale g_inh g_norm
#   g_nfast g_tf g_ts g_sig g_hidden g_fscale g_ncal g_tpc
bench() { # $1=nombre  $2=cfg  $3=g_taps  $4=extra tail  $5=filtro (nombre del config)
  local nombre="$1" cfg="$2" taps="$3" extra="$4" filtro="$5"
  local -a rows=()
  for s in "${SEEDS[@]}"; do
    # bench_bio corre BASE siempre como referencia, así que con "1,0,0,10"
    # imprime DOS lineas de holdout (BASE y JERARQ). Se filtra por nombre.
    v=$("$BBIO" data/corpus.txt data/dic.txt 200 "$s" "$cfg" \
      0 2 0.0 "$taps" 0 0 1 $extra 2>/dev/null \
      | awk -v f="$filtro" -F': ' 'index($0,f) && /holdout/{print $2}' | awk '{print $1}')
    rows+=("$(printf '%s seed %-6s %s' "$nombre" "$s" "$v")")
  done
  printf '%s\n' "${rows[@]}"
}
TAIL="0 0 0 0 0 1.0 1500 8"
BO=$(bench "BASE      " "0,0,0,10" 1 "$TAIL" "BASE")
B2=$(bench "BASE t2   " "0,0,0,10" 2 "$TAIL" "BASE")
JO=$(bench "JER300    " "1,0,0,10" 1 "300 6 200 0.8 0 1.0 1500 8" "JERARQ")
printf '%s\n%s\n%s\n' "$BO" "$B2" "$JO"

say "3. Veredicto"
B0=$(lag_of "BASE" 0); B1=$(lag_of "BASE" 1)
J0=$(lag_of "JER 300" 0); J1=$(lag_of "JER 300" 1)
avg() { printf '%s\n' "$1" | awk '{s+=$NF; n++} END{printf "%.3f", s/n}'; }
HB=$(avg "$BO"); H2=$(avg "$B2"); HJ=$(avg "$JO")

printf 'criterio de MEMORIA: lag1 >= 35%%  |  lag0 >= 95%%  |  holdout <= BASE\n'
printf '  BASE     lag0 %s%%  lag1 %s%%\n' "$B0" "$B1"
printf '  JER 300  lag0 %s%%  lag1 %s%%\n' "$J0" "$J1"
ok=1
awk -v a="$J1" 'BEGIN{exit !(a+0 >= 35.0)}' \
  && printf '  [ok]     lag1 %s%% >= 35%%\n' "$J1" \
  || { printf '  [FAIL]   lag1 %s%% < 35%%\n' "$J1"; ok=0; }
awk -v a="$J0" 'BEGIN{exit !(a+0 >= 95.0)}' \
  && printf '  [ok]     lag0 %s%% >= 95%%\n' "$J0" \
  || { printf '  [FAIL]   lag0 %s%% < 95%%\n' "$J0"; ok=0; }
awk -v a="$HJ" -v b="$HB" 'BEGIN{exit !(a+0 <= b+0 + 0.005)}' \
  && printf '  [ok]     holdout JER300 %s <= BASE %s\n' "$HJ" "$HB" \
  || { printf '  [FAIL]   holdout JER300 %s > BASE %s\n' "$HJ" "$HB"; ok=0; }

printf '\ncriterio FUNCIONAL: el lector con 2 estados pasados <= BASE\n'
printf '  promedio holdout:  BASE %s   BASE t2 %s   JER300 %s\n' "$HB" "$H2" "$HJ"
awk -v a="$H2" -v b="$HB" 'BEGIN{exit !(a+0 <= b+0 + 0.005)}' \
  && printf '  [ok]     BASE t2 %s <= BASE %s\n' "$H2" "$HB" \
  || { printf '  [FAIL]   BASE t2 %s > BASE %s\n' "$H2" "$HB"; ok=0; }

printf '\n'
[ "$ok" = 1 ] && printf '\033[32mRESULTADO: PASA.\033[0m\n' \
               || printf '\033[31mRESULTADO: NO PASA — mirá las [FAIL].\033[0m\n'
