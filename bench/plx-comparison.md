# Benchmark: PL/pgSQL interpreter compared with the uplpgsql JIT

## Purpose

This document records a benchmark that compares execution of PL/pgSQL by the
standard PostgreSQL interpreter with execution by the uplpgsql JIT. The same
PL/pgSQL source is executed under both engines and the results are compared for
equality before timings are reported.

## Source of the test functions

The test functions are written in the plx procedural language and transpiled to
PL/pgSQL. plx compiles dialect source to PL/pgSQL at `CREATE FUNCTION` time and
stores the result in `pg_proc.prosrc`. The stored PL/pgSQL is what each engine
executes.

- plx repository: https://github.com/commandprompt/plx
- plx documentation: https://commandprompt.github.io/plx/

Using plx as the source keeps the comparison at the execution layer: both
engines run byte-for-byte the same `pg_proc.prosrc`.

## Method

plx registers a call handler, `plx_call_handler`, that dispatches execution. By
default it points at the standard interpreter (`plpgsql_call_handler` in
`plpgsql`). Redefining `plx_call_handler` to point at `uplpgsql_call_handler`
routes the same stored PL/pgSQL through the uplpgsql JIT:

```sql
-- interpreter
CREATE OR REPLACE FUNCTION plx_call_handler() RETURNS language_handler
  AS '$libdir/plpgsql', 'plpgsql_call_handler' LANGUAGE C;

-- uplpgsql JIT
CREATE OR REPLACE FUNCTION plx_call_handler() RETURNS language_handler
  AS '$libdir/uplpgsql', 'uplpgsql_call_handler' LANGUAGE C;
```

Each workload was measured as follows:

1. Select the engine by redefining `plx_call_handler`.
2. Issue one warm-up call so uplpgsql compiles the function and caches it.
3. Run `EXPLAIN ANALYZE` on the call several times in one session.
4. Record the minimum reported `Execution Time`.

The warm-up and the measured runs share one session, so uplpgsql timings reflect
the cached compiled function and do not include first-call compilation.

## Environment

- PostgreSQL 20devel
- LLVM 21
- Ubuntu 26.04, x86-64
- uplpgsql built with all current fixes applied

## Workloads

The workloads are numeric procedures written in the plx `plsql` dialect. Each
uses variables and loops only, with no array or SPI access, so execution stays
on the procedural path.

| Name | Description |
|------|-------------|
| int4 arithmetic | Accumulate `i` modulo a constant over a counted loop |
| int8 arithmetic | Accumulate `i*i` modulo a constant over a counted loop |
| float8 arithmetic | Accumulate `1.0/(i*i)` over a counted loop |
| branch and nested loop | Total Collatz step count for 1 through n |
| nested loop | Fixed-point Mandelbrot escape-time checksum over a square grid |

Definitions are in [`plx_workloads.sql`](plx_workloads.sql). The runner is
[`run.sh`](run.sh).

## Results

Minimum `Execution Time` over the measured runs. Every workload returned an
identical value under both engines.

| Workload | Interpreter (ms) | uplpgsql JIT (ms) | Ratio |
|----------|-----------------:|------------------:|------:|
| int4 arithmetic          | 2450.6 | 118.0 | 20.8 |
| nested loop (Mandelbrot) |  213.3 |  11.2 | 19.1 |
| int8 arithmetic          | 1273.2 | 130.4 |  9.8 |
| branch and nested loop   | 6143.0 | 641.2 |  9.6 |
| float8 arithmetic        | 5846.0 | 5017.4 |  1.2 |

Return values checked for equality across engines:

| Workload | Call | Value |
|----------|------|-------|
| int8 arithmetic | `w_i8(2000000)` | 319464 |
| int4 arithmetic | `w_i4(3000000)` | 0 |
| float8 arithmetic | `w_f8(1000000)` | 1.64493306684877 |
| branch and nested loop | `w_coll(50000)` | 5025114 |
| nested loop | `w_mand(100)` | 5717464 |

## Notes

- The integer and control-flow workloads run between 9.6 and 20.8 times faster
  under the JIT. These paths compile to native arithmetic, branches, and loops.
- The float8 workload runs 1.2 times faster. That loop is dominated by division
  and type conversion that use the runtime fallback rather than native IR, so the
  procedural speedup is smaller.

## Reproduction

With PostgreSQL 20devel running, plx and uplpgsql installed, and both extensions
created in the target database:

```sh
psql -d <db> -f bench/plx_workloads.sql
DB=<db> bench/run.sh
```

`run.sh` prints a table of interpreter and JIT timings and the computed ratio.
