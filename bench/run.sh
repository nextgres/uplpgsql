#!/usr/bin/env bash
#
# Compare the PL/pgSQL interpreter with the uplpgsql JIT.
#
# Both engines run byte-identical source: each workload body is installed
# under both languages from one string (see workloads.sql), so the only
# variable is the engine.
#
# Every workload's return value is checked for equality across the two engines
# before its timing is reported.  A row that does not agree is reported as
# MISMATCH and its timings are suppressed -- they would not mean anything.
#
# Warm-up and measurement share one session, so uplpgsql timings reflect the
# cached compiled function rather than first-call compilation.  The minimum
# Execution Time over RUNS is reported, which is the usual choice for
# comparing best-case throughput rather than scheduler noise.
#
# Prerequisites: uplpgsql installed, workloads loaded from workloads.sql.
#
# Usage: DB=mydb [PSQL_CONN="-p 5432"] [RUNS=5] bench/run.sh

set -uo pipefail

DB="${DB:-plxbench}"
PSQL="psql -X ${PSQL_CONN:-} -d $DB"
RUNS="${RUNS:-5}"

# workload | arg  -- keep the label short, it is a table column
WL=(
  "int4 arithmetic|i4|20000000"
  "int8 arithmetic|i8|10000000"
  "numeric arithmetic|num|2000000"
  "float8 arithmetic|f8|10000000"
  "branch and nested loop|coll|300000"
  "nested loop (Mandelbrot)|mand|200"
  "array element reads|arr|5000000"
  "array element writes|arrw|200000"
)

# Correctness arguments: small enough to compare cheaply.
declare -A CHECK_ARG=(
  [i4]=3000000 [i8]=2000000 [num]=1000000 [f8]=1000000
  [coll]=50000 [mand]=100 [arr]=100000 [arrw]=5000
)

value() {  # $1 = fn suffix, $2 = lang, $3 = arg
  $PSQL -t -A -c "SELECT w_${1}_${2}(${3});" 2>&1 | tr -d '[:space:]'
}

measure() {  # $1 = fn suffix, $2 = lang, $3 = arg -- prints minimum ms
  {
    echo "SELECT w_${1}_${2}(${3});"
    for _ in $(seq 1 "$RUNS"); do echo "EXPLAIN ANALYZE SELECT w_${1}_${2}(${3});"; done
  } | $PSQL 2>/dev/null | grep 'Execution Time' | grep -oE '[0-9.]+' | sort -g | head -1
}

printf '%-26s %14s %14s %8s\n' "workload" "interpreter(ms)" "uplpgsql(ms)" "ratio"
printf '%-26s %14s %14s %8s\n' "--------------------------" "--------------" "--------------" "--------"

fail=0
for w in "${WL[@]}"; do
  IFS='|' read -r label fn arg <<< "$w"

  # Agree before timing.
  vp="$(value "$fn" plpgsql  "${CHECK_ARG[$fn]}")"
  vu="$(value "$fn" uplpgsql "${CHECK_ARG[$fn]}")"
  if [ -z "$vp" ] || [ "$vp" != "$vu" ]; then
    printf '%-26s %s\n' "$label" "MISMATCH (plpgsql=[$vp] uplpgsql=[$vu]) -- not timed"
    fail=1
    continue
  fi

  p="$(measure "$fn" plpgsql "$arg")"
  u="$(measure "$fn" uplpgsql "$arg")"
  if [ -z "$p" ] || [ -z "$u" ]; then
    printf '%-26s %s\n' "$label" "MEASUREMENT FAILED -- not reported"
    fail=1
    continue
  fi

  ratio="$(awk -v a="$p" -v b="$u" 'BEGIN{ if (b+0>0) printf "%.1f", a/b; else print "n/a" }')"
  printf '%-26s %14s %14s %7sx\n' "$label" "$p" "$u" "$ratio"
done

exit $fail
