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
- uplpgsql on `main`, including the constant-fold, eval-scope, and
  FunctionCallInfo-hoist fixes

Ratios are the point of comparison; absolute times are machine-specific.

## Results

| Workload | Interpreter (ms) | uplpgsql JIT (ms) | Ratio |
|----------|-----------------:|------------------:|------:|
| nested loop (Mandelbrot)   |  119.5 |   5.2 | 23.0x |
| float8 arithmetic          |  783.8 |  36.1 | 21.7x |
| array element reads        |  349.4 |  21.1 | 16.6x |
| Cornell box path tracer    | 43953.9 | 2887.9 | 15.2x |
| int4 arithmetic            | 1408.3 |  95.8 | 14.7x |
| branch and nested loop     | 3429.7 | 263.1 | 13.0x |
| int8 arithmetic            |  767.7 |  84.9 |  9.0x |
| array element writes       |   25.6 |   5.1 |  5.0x |
| numeric arithmetic         |  502.9 | 355.8 |  1.4x |

All workloads returned identical values under both engines.

## Reading the table

**Scalar arithmetic, branches and loops compile to native code**, and run 9x to
23x faster. This is what the JIT is for.

**numeric has no native path.** It is included because it is easy to write a
benchmark that believes it is measuring float8 and is not: in PostgreSQL an
unadorned `1.0` is numeric, so

```sql
s := s + 1.0/(i*i);        -- numeric division, whatever s is declared as
s := s + 1.0::float8/(i*i)::float8;   -- float8
```

are different workloads. The first is the 1.3x row; the second is the 21.8x
row. Both appear above under their real names.

**Array element writes are the lowest-ratio compiled row, and the reason is
the value expression, not the store.** Separating the two halves:

| | interpreter | uplpgsql | ratio |
|---|---:|---:|---:|
| `t[i] := i` — Tier 1 value, native store | 20.0 ms | 2.0 ms | 10.3x |
| `floor(power(v,0.45)*255.0+0.5)::int` — no array at all | 10.6 ms | 4.3 ms | 2.5x |

The native store is fine. The value expression is where the gap is: it is a
tree of Tier 2 calls (`power`, `floor`, the cast), and Tier 2 does not compile
as tightly as Tier 1's native arithmetic. It still beats the interpreter, but
2.5x against 10x for the store, so the mixed workload lands in between.

Two Tier 2 costs, both since closed, had held this row down. First, `float8`
casts of numeric literals were emitted as per-call `float8(numeric)`
conversions — measured at 0.3x, slower than the interpreter, and leaking memory
besides — until they were folded to constants at compile time. Second, the
`FunctionCallInfo` was allocated once but zeroed and re-stamped with its
loop-invariant `flinfo`/`nargs`/`fncollation` on every call; that setup now
happens once per function invocation in the entry block, leaving only the
argument stores and the isnull reset per call — the same shape PostgreSQL's own
interpreter uses. Together those moved this row from 0.8x to 5.0x. What remains
is the irreducible cost of a real function call per operator, which is why a
call-dominated expression still trails native Tier 1 arithmetic.

**The Cornell box** (`cornell.sql`) is the closest thing here to real code:
float8 throughout, a scene held in arrays read once per sphere per bounce,
nested loops, branches, a shadow ray cast toward the light at every diffuse
bounce (next-event estimation), and a deterministic image that both engines
must agree on pixel for pixel. It is the workload that has found the most bugs
— the quadratic array write, the whole-array marshal on fallback, the Tier 2
gap above, and a per-call float8(numeric) memory leak were all first seen here.
The row is measured at 64x64, 100 samples per pixel; the ratio rises with the
sample count, since more samples spend proportionally more time in the loop
arithmetic the JIT compiles and less in fixed per-call overhead.

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
