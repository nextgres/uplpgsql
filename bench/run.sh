#!/usr/bin/env bash
#
# Benchmark the plx workloads under the PL/pgSQL interpreter and the uplpgsql
# JIT. The engine is selected by redefining plx_call_handler. Warm-up and
# measurement share one session, so uplpgsql timings reflect the cached
# compiled function. The minimum Execution Time over RUNS is reported.
#
# Prerequisites: plx and uplpgsql installed; both extensions created in $DB;
# workloads loaded from plx_workloads.sql.
#
# Usage: DB=mydb [PSQL_CONN="-h /tmp -p 5433 -U postgres"] bench/run.sh

set -u
DB="${DB:-plxbench}"
PSQL="psql -X ${PSQL_CONN:-} -d $DB"
RUNS="${RUNS:-5}"

set_handler() {  # $1 = plpgsql | uplpgsql
  $PSQL -c "CREATE OR REPLACE FUNCTION plx_call_handler() RETURNS language_handler AS '\$libdir/$1','${1}_call_handler' LANGUAGE C;" >/dev/null 2>&1
}

measure() {  # $1 = call expression; prints minimum ms
  {
    echo "SELECT $1;"
    for r in $(seq 1 "$RUNS"); do echo "EXPLAIN ANALYZE SELECT $1;"; done
  } | $PSQL 2>/dev/null | grep "Execution Time" | grep -oE "[0-9.]+" | sort -g | head -1
}

declare -a WL=(
  "int4 arithmetic|w_i4(20000000)"
  "int8 arithmetic|w_i8(10000000)"
  "float8 arithmetic|w_f8(10000000)"
  "branch and nested loop|w_coll(300000)"
  "nested loop (Mandelbrot)|w_mand(200)"
)

printf "%-26s %14s %14s %8s\n" "workload" "interpreter(ms)" "uplpgsql(ms)" "ratio"
for w in "${WL[@]}"; do
  label="${w%%|*}"; call="${w##*|}"
  set_handler plpgsql;  p="$(measure "$call")"
  set_handler uplpgsql; u="$(measure "$call")"
  ratio="$(awk -v a="$p" -v b="$u" 'BEGIN{ if (b>0) printf "%.1f", a/b; else print "n/a" }')"
  printf "%-26s %14s %14s %8s\n" "$label" "$p" "$u" "$ratio"
done

set_handler plpgsql
