# uplpgsql

A JIT-compiling PL/pgSQL for Postgres, derived from the NEXTGRES Universal Procedural Language (UPL) compiler and runtime.

> [!WARNING]
> This is a pre-alpha work-in-progress predicated on an AI-assisted analysis and JIT-oriented reconstruction of the UPL compiler and runtime. Do not use it in production. Interfaces and behaviour will change without notice. Crashes, correctness issues, and data loss are very likely.

## What it is

`uplpgsql` is a procedural language handler. Functions declared `LANGUAGE uplpgsql` are compiled to native machine code on first execution and run as native code thereafter. The language they are written in is PL/pgSQL — the same grammar, the same semantics, the same error messages.

```sql
CREATE EXTENSION uplpgsql;

CREATE OR REPLACE FUNCTION mandelbrot_fixed_point(
    p_width      integer DEFAULT 800,
    p_height     integer DEFAULT 600,
    p_iterations integer DEFAULT 500
)
RETURNS bigint
LANGUAGE uplpgsql
IMMUTABLE
STRICT
AS $$
DECLARE
    scale_constant CONSTANT bigint := 65536;

    real_min CONSTANT bigint := -2 * 65536;
    real_max CONSTANT bigint :=  1 * 65536;

    imag_min CONSTANT bigint := (-12 * 65536) / 10;
    imag_max CONSTANT bigint := ( 12 * 65536) / 10;

    escape_radius_squared CONSTANT bigint := 4 * 65536;

    pixel_x integer;
    pixel_y integer;
    iteration integer;

    c_real bigint;
    c_imag bigint;

    z_real bigint;
    z_imag bigint;

    z_real_squared bigint;
    z_imag_squared bigint;

    next_real bigint;

    checksum bigint := 0;
BEGIN
    IF p_width < 2 THEN
        RAISE EXCEPTION 'Width must be at least 2';
    END IF;

    IF p_height < 2 THEN
        RAISE EXCEPTION 'Height must be at least 2';
    END IF;

    IF p_iterations < 1 THEN
        RAISE EXCEPTION 'Iterations must be positive';
    END IF;

    pixel_y := 0;

    WHILE pixel_y < p_height LOOP
        c_imag :=
            imag_min
            + (
                (imag_max - imag_min) * pixel_y
                / (p_height - 1)
            );

        pixel_x := 0;

        WHILE pixel_x < p_width LOOP
            c_real :=
                real_min
                + (
                    (real_max - real_min) * pixel_x
                    / (p_width - 1)
                );

            z_real := 0;
            z_imag := 0;
            iteration := 0;

            WHILE iteration < p_iterations LOOP
                z_real_squared :=
                    (z_real * z_real) / scale_constant;

                z_imag_squared :=
                    (z_imag * z_imag) / scale_constant;

                IF z_real_squared + z_imag_squared
                       > escape_radius_squared THEN
                    EXIT;
                END IF;

                next_real :=
                    z_real_squared
                    - z_imag_squared
                    + c_real;

                z_imag :=
                    (
                        (2 * z_real * z_imag)
                        / scale_constant
                    )
                    + c_imag;

                z_real := next_real;
                iteration := iteration + 1;
            END LOOP;

            checksum :=
                (
                    checksum
                    + iteration * 31::bigint
                    + pixel_x * 17::bigint
                    + pixel_y * 13::bigint
                ) % 9223372036854775783::bigint;

            pixel_x := pixel_x + 1;
        END LOOP;

        pixel_y := pixel_y + 1;
    END LOOP;

    RETURN checksum;
END;
$$;
```

## How it differs from PL/pgSQL

PostgreSQL's PL/pgSQL is a tree-walking interpreter. Every `IF`, every loop iteration, every assignment costs a switch dispatch and a recursive call through `exec_stmt`. PostgreSQL's own JIT does not help: it compiles SQL expressions and tuple deforming, not procedural control flow.  `uplpgsql` compiles the control flow itself. Statements become LLVM basic blocks and branches; loops become real loops; variable access becomes a load from a computed struct offset. Compiled functions are cached per backend and invalidated on `fn_xmin`/`fn_tid` change, so a `CREATE OR REPLACE` recompiles and nothing stale survives.

How it works:

**Native IR first.** Control flow, arithmetic, and datum access are emitted as instructions. Only operations that inherently require PostgreSQL's C infrastructure — SPI, expression evaluation, tuplestores, subtransactions — delegate to runtime helpers.

**Three-tier expressions.** Integer and float arithmetic compile to overflow intrinsics with no call overhead. Other operators resolve their C function pointer at compile time and bypass fmgr. Anything left falls back to a runtime helper. The fallback is the escape hatch, not the plan.

Everything else is unchanged. The GUCs are mirrored under the `uplpgsql.` prefix (`variable_conflict`, `print_strict_params`, `check_asserts`, `extra_warnings`, `extra_errors`), joined by `uplpgsql.log_compilation`, `uplpgsql.dump_ir`, and `uplpgsql.enable_jit_heuristic`. Porting a function is a one-word edit.

## Universal procedural language

PL/pgSQL is one front-end, not the point. The compiler underneath — UPL — is language-agnostic: it knows about basic blocks, branches, datums, and a JIT, and nothing about any particular procedural dialect.

```
core/     libupl_core.a    LLVM lifecycle, OrcJIT, function cache,
                           IR primitives, datum access
common/   shared PL infra  parser scaffolding, interpreter fallback,
                           statement and expression compilation, runtime helpers
drivers/  one per language grammar, scanner, call handler, extension
```

A driver owns its parser, its AST, and its interpreter fallback. It reaches the JIT only through `upl_emit_*()`. The core never names a language-specific type.  This is what makes the other front-ends tractable: fork the dialect's existing parser, walk its AST, emit through the same primitives.

| Front-end | Status |
|-----------|--------|
| PL/pgSQL  | Working. Tracks PostgreSQL 20devel; 14/14 regression tests pass. |
| SQL/PSM   | Removed. Adds `SIGNAL`/`RESIGNAL`, `REPEAT`, `DECLARE HANDLER`. |
| PL/SQL    | Removed. Adds packages, nested subprograms, collections. |
| T-SQL     | Removed. Adds `TRY`/`CATCH`, `GOTO`, `RAISERROR`. |

Only the PL/pgSQL driver lives in this tree today.

## Building

Requires PostgreSQL 20devel and LLVM 15+, and is developed/tested against LLVM 22. The build is C only and uses the LLVM C API only.

PostgreSQL does **not** need to be built `--with-llvm`. `uplpgsql` links its own LLVM and owns its LLJIT instance; it never touches PostgreSQL's `llvmjit` provider. A stock server with no JIT support of its own runs JIT-compiled PL/pgSQL just fine.

```sh
make install
make installcheck
```

PGXS does not track header dependencies. After editing anything in `core/` or `common/`, run `make clean && make install` — a stale object file with the old struct layout will not fail to link, it will fail at runtime.

## License

Apache 2.0

