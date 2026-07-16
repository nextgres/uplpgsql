# Benchmark: PL/pgSQL interpreter compared with the uplpgsql JIT

## Method

Each workload body is written once and installed under both languages, so the
two engines execute byte-identical source:

```sql
EXECUTE format('CREATE FUNCTION w_i4_%s(n int) RETURNS int AS %L LANGUAGE %s',
               lang, body, lang);
```

That is the whole methodological requirement — the engine is the only variable
— and it needs nothing beyond `format()`. An earlier version of this benchmark
obtained the same property by writing the workloads in [plx](https://github.com/commandprompt/plx)
and redefining `plx_call_handler` to point at one engine or the other. That
works, but it makes the numbers depend on a third-party extension and on which
of its dialects a given build ships, for no gain over installing one string
twice.

Every workload's return value is checked for equality across the two engines
before any timing is reported. A row that disagrees is reported as MISMATCH and
its timings are suppressed. Warm-up and measurement share one session, so
uplpgsql timings reflect the cached compiled function rather than first-call
compilation. The minimum `Execution Time` over five runs is reported.

## Environment

- PostgreSQL 20devel, LLVM 22.1.7
- Apple M2 Max, macOS/arm64
- uplpgsql at `a4cec41`

Ratios are the point of comparison; absolute times are machine-specific.

## Results

| Workload | Interpreter (ms) | uplpgsql JIT (ms) | Ratio |
|----------|-----------------:|------------------:|------:|
| float8 arithmetic          |  769.8 |  35.4 | 21.8x |
| nested loop (Mandelbrot)   |  118.3 |   5.8 | 20.4x |
| array element reads        |  340.0 |  20.8 | 16.4x |
| int4 arithmetic            | 1393.6 |  95.9 | 14.5x |
| branch and nested loop     | 3376.6 | 338.8 | 10.0x |
| int8 arithmetic            |  755.4 |  92.7 |  8.1x |
| numeric arithmetic         |  493.9 | 386.1 |  1.3x |
| array element writes       |   25.0 |  31.7 |  0.8x |
| Cornell box path tracer    |  567.6 |  67.9 |  8.4x |

All workloads returned identical values under both engines.

## Reading the table

**Scalar arithmetic, branches and loops compile to native code**, and run 8x to
22x faster. This is what the JIT is for.

**numeric has no native path.** It is included because it is easy to write a
benchmark that believes it is measuring float8 and is not: in PostgreSQL an
unadorned `1.0` is numeric, so

```sql
s := s + 1.0/(i*i);        -- numeric division, whatever s is declared as
s := s + 1.0::float8/(i*i)::float8;   -- float8
```

are different workloads. The first is the 1.3x row; the second is the 21.8x
row. Both appear above under their real names.

**Array element writes are at parity, and the array is not the reason.**
Separating the two halves of that workload:

| | interpreter | uplpgsql | ratio |
|---|---:|---:|---:|
| `t[i] := i` — Tier 1 value, native store | 20.0 ms | 2.0 ms | 10.3x |
| `floor(power(v,0.45)*255.0+0.5)::int` — no array at all | 10.6 ms | 31.7 ms | **0.3x** |

The native store is fine. The value expression is the problem: it needs three
Tier 2 calls (`power`, `floor`, and the cast), and Tier 2 is currently slower
than the interpreter. The `FunctionCallInfo` is allocated once in the entry
block, but it is zeroed and its `flinfo`/`context`/`resultinfo`/`fncollation`/
`nargs` fields are rewritten on every call, all of which are loop-invariant.
PostgreSQL's own expression interpreter initialises its `FunctionCallInfo` once
when the `ExprState` is built and writes only the arguments per call. Hoisting
that initialisation is an open optimisation; until it lands, an expression
dominated by function calls gains nothing from being compiled.

**The Cornell box** (`cornell.sql`) is the closest thing here to real code:
float8 throughout, a scene held in arrays read once per sphere per bounce,
nested loops, branches, and a deterministic image that both engines must agree
on pixel for pixel. It is the workload that has found the most bugs.

## Reproduction

```sh
createdb bench && psql -d bench -c 'CREATE EXTENSION uplpgsql'
psql -d bench -f bench/workloads.sql
DB=bench bench/run.sh

psql -d bench -f bench/cornell.sql
psql -d bench -c 'SELECT cornell_uplpgsql(24,4) = cornell_plpgsql(24,4)'
```

`run.sh` exits non-zero if any workload's engines disagree or a measurement
fails, so it is safe to run unattended.
