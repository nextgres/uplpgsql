--
-- plpgsql_coverage
--
-- Surfaces the compiler supports but nothing else exercised: GET DIAGNOSTICS,
-- DO blocks (the inline handler), RETURNS TABLE, integer overflow edge cases,
-- the extra_warnings/extra_errors GUCs, and event triggers.
--
-- Every expected value here was taken from stock PL/pgSQL running the same
-- body; uplpgsql must agree with the interpreter it replaces.
--

--
-- GET DIAGNOSTICS
--
CREATE TABLE cov_gd(i int);

CREATE FUNCTION cov_rowcount() RETURNS int LANGUAGE uplpgsql AS $$
DECLARE n int;
BEGIN
  INSERT INTO cov_gd SELECT generate_series(1,3);
  GET DIAGNOSTICS n = ROW_COUNT;
  RETURN n;
END; $$;
SELECT cov_rowcount();

CREATE FUNCTION cov_rowcount_upd() RETURNS int LANGUAGE uplpgsql AS $$
DECLARE n int;
BEGIN
  UPDATE cov_gd SET i = i + 1;
  GET DIAGNOSTICS n = ROW_COUNT;
  RETURN n;
END; $$;
SELECT cov_rowcount_upd();

-- ROW_COUNT after a no-op
CREATE FUNCTION cov_rowcount_zero() RETURNS int LANGUAGE uplpgsql AS $$
DECLARE n int;
BEGIN
  DELETE FROM cov_gd WHERE i = 9999;
  GET DIAGNOSTICS n = ROW_COUNT;
  RETURN n;
END; $$;
SELECT cov_rowcount_zero();

CREATE FUNCTION cov_pg_context() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE s text;
BEGIN
  GET DIAGNOSTICS s = PG_CONTEXT;
  RETURN CASE WHEN s IS NULL THEN 'null' ELSE 'has-context' END;
END; $$;
SELECT cov_pg_context();

-- GET STACKED DIAGNOSTICS inside a handler
CREATE FUNCTION cov_stacked() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE msg text; state text;
BEGIN
  RAISE EXCEPTION 'boom-%', 42;
EXCEPTION WHEN others THEN
  GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT, state = RETURNED_SQLSTATE;
  RETURN msg || '/' || state;
END; $$;
SELECT cov_stacked();

-- SQLSTATE of a real (non-RAISE) error, read in the handler
CREATE FUNCTION cov_stacked_div() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE state text; a int := 1; b int := 0; r int;
BEGIN
  r := a / b;
  RETURN 'no-error';
EXCEPTION WHEN others THEN
  GET STACKED DIAGNOSTICS state = RETURNED_SQLSTATE;
  RETURN state;
END; $$;
SELECT cov_stacked_div();

--
-- DO blocks (uplpgsql_inline_handler)
--
DO LANGUAGE uplpgsql $$
BEGIN
  RAISE NOTICE 'do: %', 1 + 1;
END $$;

-- the inline handler's error path
DO LANGUAGE uplpgsql $$
BEGIN
  RAISE EXCEPTION 'do-error';
EXCEPTION WHEN others THEN
  RAISE NOTICE 'do caught: %', SQLERRM;
END $$;

-- a DO block that declares and loops
DO LANGUAGE uplpgsql $$
DECLARE t int := 0;
BEGIN
  FOR i IN 1..4 LOOP t := t + i; END LOOP;
  RAISE NOTICE 'do sum: %', t;
END $$;

-- an uncaught error out of a DO block
DO LANGUAGE uplpgsql $$
BEGIN
  RAISE EXCEPTION 'do-uncaught';
END $$;

--
-- RETURNS TABLE / SETOF
--
CREATE FUNCTION cov_table(n int) RETURNS TABLE(id int, lbl text) LANGUAGE uplpgsql AS $$
BEGIN
  FOR i IN 1..n LOOP
    id := i;
    lbl := 'r' || i;
    RETURN NEXT;
  END LOOP;
END; $$;
SELECT * FROM cov_table(3);

-- RETURNS TABLE fed by RETURN QUERY
CREATE FUNCTION cov_table_query(n int) RETURNS TABLE(id int) LANGUAGE uplpgsql AS $$
BEGIN
  RETURN QUERY SELECT g FROM generate_series(1,n) g;
END; $$;
SELECT * FROM cov_table_query(3);

-- SETOF composite
CREATE TYPE cov_pair AS (a int, b text);
CREATE FUNCTION cov_setof() RETURNS SETOF cov_pair LANGUAGE uplpgsql AS $$
DECLARE r cov_pair;
BEGIN
  r.a := 1; r.b := 'x'; RETURN NEXT r;
  r.a := 2; r.b := 'y'; RETURN NEXT r;
END; $$;
SELECT * FROM cov_setof();

-- RETURN QUERY EXECUTE
CREATE FUNCTION cov_return_query_exec() RETURNS SETOF int LANGUAGE uplpgsql AS $$
BEGIN
  RETURN QUERY EXECUTE 'select g from generate_series(1,3) g';
END; $$;
SELECT * FROM cov_return_query_exec();

--
-- Integer overflow / INT_MIN edge cases.
--
-- These compile to overflow intrinsics; INT_MIN / -1 and INT_MIN % -1 are the
-- cases where the hardware instruction traps, so they must be handled without
-- crashing the backend.
--
CREATE FUNCTION cov_int4_min_div() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a int := -2147483648; b int := -1; r int;
BEGIN
  r := a / b;
  RETURN r::text;
EXCEPTION WHEN others THEN RETURN SQLSTATE;
END; $$;
SELECT cov_int4_min_div();

CREATE FUNCTION cov_int4_min_mod() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a int := -2147483648; b int := -1; r int;
BEGIN
  r := a % b;
  RETURN r::text;
EXCEPTION WHEN others THEN RETURN SQLSTATE;
END; $$;
SELECT cov_int4_min_mod();

CREATE FUNCTION cov_int8_min_div() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a bigint := -9223372036854775808; b bigint := -1; r bigint;
BEGIN
  r := a / b;
  RETURN r::text;
EXCEPTION WHEN others THEN RETURN SQLSTATE;
END; $$;
SELECT cov_int8_min_div();

CREATE FUNCTION cov_int8_min_mod() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a bigint := -9223372036854775808; b bigint := -1; r bigint;
BEGIN
  r := a % b;
  RETURN r::text;
EXCEPTION WHEN others THEN RETURN SQLSTATE;
END; $$;
SELECT cov_int8_min_mod();

CREATE FUNCTION cov_int4_neg_min() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a int := -2147483648; r int;
BEGIN
  r := -a;
  RETURN r::text;
EXCEPTION WHEN others THEN RETURN SQLSTATE;
END; $$;
SELECT cov_int4_neg_min();

CREATE FUNCTION cov_int4_add_ovf() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a int := 2147483647; b int := 1; r int;
BEGIN
  r := a + b;
  RETURN r::text;
EXCEPTION WHEN numeric_value_out_of_range THEN RETURN 'ovf';
END; $$;
SELECT cov_int4_add_ovf();

CREATE FUNCTION cov_int4_mul_ovf() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a int := 2147483647; b int := 2; r int;
BEGIN
  r := a * b;
  RETURN r::text;
EXCEPTION WHEN numeric_value_out_of_range THEN RETURN 'ovf';
END; $$;
SELECT cov_int4_mul_ovf();

CREATE FUNCTION cov_int4_div_zero() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a int := 1; b int := 0; r int;
BEGIN
  r := a / b;
  RETURN r::text;
EXCEPTION WHEN division_by_zero THEN RETURN 'div0';
END; $$;
SELECT cov_int4_div_zero();

CREATE FUNCTION cov_int4_mod_zero() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a int := 1; b int := 0; r int;
BEGIN
  r := a % b;
  RETURN r::text;
EXCEPTION WHEN division_by_zero THEN RETURN 'mod0';
END; $$;
SELECT cov_int4_mod_zero();

--
-- extra_warnings / extra_errors
--
-- These are applied by the validator, which must compile with forValidator
-- true; otherwise do_compile() zeroes both and the checks silently never run.
--
SET uplpgsql.extra_warnings TO 'shadowed_variables';
-- should WARN
CREATE FUNCTION cov_shadow_warn() RETURNS int LANGUAGE uplpgsql AS $$
DECLARE x int := 1;
BEGIN
  DECLARE x int := 2;
  BEGIN
    RETURN x;
  END;
END; $$;
RESET uplpgsql.extra_warnings;

SET uplpgsql.extra_errors TO 'shadowed_variables';
-- should ERROR
CREATE FUNCTION cov_shadow_err() RETURNS int LANGUAGE uplpgsql AS $$
DECLARE x int := 1;
BEGIN
  DECLARE x int := 2;
  BEGIN
    RETURN x;
  END;
END; $$;
RESET uplpgsql.extra_errors;

-- no shadowing: must stay silent even with the check on
SET uplpgsql.extra_errors TO 'shadowed_variables';
CREATE FUNCTION cov_noshadow() RETURNS int LANGUAGE uplpgsql AS $$
DECLARE x int := 1;
BEGIN
  DECLARE y int := 2;
  BEGIN
    RETURN x + y;
  END;
END; $$;
SELECT cov_noshadow();
RESET uplpgsql.extra_errors;

-- extra_errors is a CREATE-time (validator) check, not an execution-time one.
-- The call handler must pass forValidator = false: passing the trigger flag
-- there compiled every trigger in validator mode on each fire, so a plain DML
-- statement failed whenever this GUC happened to be set.
CREATE TABLE cov_trg_t(i int);
CREATE FUNCTION cov_trg_shadow() RETURNS trigger LANGUAGE uplpgsql AS $$
DECLARE x int := 1;
BEGIN
  DECLARE x int := 2;
  BEGIN
    RETURN new;
  END;
END; $$;
CREATE TRIGGER cov_trg BEFORE INSERT ON cov_trg_t
  FOR EACH ROW EXECUTE FUNCTION cov_trg_shadow();

-- firing the trigger with the check on must NOT error
SET uplpgsql.extra_errors TO 'shadowed_variables';
INSERT INTO cov_trg_t VALUES (1);
RESET uplpgsql.extra_errors;
SELECT count(*) AS trg_rows FROM cov_trg_t;

DROP TABLE cov_trg_t CASCADE;
DROP FUNCTION cov_trg_shadow();

-- strict_multi_assignment: too many source columns
SET uplpgsql.extra_errors TO 'strict_multi_assignment';
CREATE FUNCTION cov_strict_multi() RETURNS int LANGUAGE uplpgsql AS $$
DECLARE a int; b int;
BEGIN
  SELECT 1, 2, 3 INTO a, b;
  RETURN a;
END; $$;
SELECT cov_strict_multi();

-- too few source columns
CREATE FUNCTION cov_strict_multi_few() RETURNS int LANGUAGE uplpgsql AS $$
DECLARE a int; b int;
BEGIN
  SELECT 1 INTO a, b;
  RETURN a;
END; $$;
SELECT cov_strict_multi_few();
RESET uplpgsql.extra_errors;

-- with the check off, the same function is fine
CREATE FUNCTION cov_lax_multi() RETURNS int LANGUAGE uplpgsql AS $$
DECLARE a int; b int;
BEGIN
  SELECT 1, 2, 3 INTO a, b;
  RETURN a;
END; $$;
SELECT cov_lax_multi();

--
-- STRICT/NULL propagation out of a nested Tier-2 call.
--
-- fmgr_load_arg_isnull() used to answer a constant "not null" for any argument
-- that was not a Const/Param/RelabelType, so a NULL produced by a nested call
-- was invisible to the enclosing strict function.
--
CREATE FUNCTION cov_nested_null() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a int; b int; r int;
BEGIN
  r := abs(int4larger(a, b));
  RETURN coalesce(r::text, 'NULL');
END; $$;
SELECT cov_nested_null();

CREATE FUNCTION cov_nested_null2() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a int; r int;
BEGIN
  r := abs(int4larger(a, 7));
  RETURN coalesce(r::text, 'NULL');
END; $$;
SELECT cov_nested_null2();

CREATE FUNCTION cov_nested_deep() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a int; r int;
BEGIN
  r := abs(int4larger(int4smaller(a, 1), 2));
  RETURN coalesce(r::text, 'NULL');
END; $$;
SELECT cov_nested_deep();

-- the same shapes with no NULL must still compute
CREATE FUNCTION cov_nested_ok() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a int := 3; r int;
BEGIN
  r := abs(int4larger(a, 7));
  RETURN r::text;
END; $$;
SELECT cov_nested_ok();

CREATE FUNCTION cov_nested_deep_ok() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a int := 5; r int;
BEGIN
  r := abs(int4larger(int4smaller(a, 1), 2));
  RETURN r::text;
END; $$;
SELECT cov_nested_deep_ok();

-- a nested NULL reaching a boolean condition
CREATE FUNCTION cov_nested_null_cond() RETURNS text LANGUAGE uplpgsql AS $$
DECLARE a int; b int;
BEGIN
  IF abs(int4larger(a, b)) > 0 THEN RETURN 'T'; ELSE RETURN 'F-or-N'; END IF;
END; $$;
SELECT cov_nested_null_cond();

--
-- Every exit path of a BEGIN ... EXCEPTION block.
--
-- The exception frame is heap-allocated on entry (upstream uses a stack-local
-- sigjmp_buf via PG_TRY and has nothing to free), so exactly one terminal
-- helper must release it: try_exit, handler_done or rethrow.  A RETURN inside
-- a handler body has to reach handler_done too, or it bypasses the cleanup.
--
CREATE FUNCTION cov_exc_normal() RETURNS int LANGUAGE uplpgsql AS $$
DECLARE x int := 0;
BEGIN
  BEGIN x := 1; EXCEPTION WHEN others THEN x := 9; END;
  RETURN x;
END; $$;
SELECT cov_exc_normal();

CREATE FUNCTION cov_exc_caught() RETURNS int LANGUAGE uplpgsql AS $$
DECLARE x int := 0;
BEGIN
  BEGIN RAISE EXCEPTION 'e'; EXCEPTION WHEN others THEN x := 9; END;
  RETURN x;
END; $$;
SELECT cov_exc_caught();

CREATE FUNCTION cov_exc_ret_try() RETURNS text LANGUAGE uplpgsql AS $$
BEGIN
  BEGIN RETURN 'ret-in-try'; EXCEPTION WHEN others THEN RETURN 'h'; END;
END; $$;
SELECT cov_exc_ret_try();

-- RETURN inside a handler: must still run HANDLER_DONE
CREATE FUNCTION cov_exc_ret_handler() RETURNS text LANGUAGE uplpgsql AS $$
BEGIN
  BEGIN
    RAISE EXCEPTION 'e';
  EXCEPTION WHEN others THEN
    RETURN 'ret-in-handler';
  END;
  RETURN 'fell-through';
END; $$;
SELECT cov_exc_ret_handler();

CREATE FUNCTION cov_exc_rethrow() RETURNS text LANGUAGE uplpgsql AS $$
BEGIN
  BEGIN
    BEGIN RAISE EXCEPTION division_by_zero;
    EXCEPTION WHEN unique_violation THEN RETURN 'wrong'; END;
  EXCEPTION WHEN others THEN RETURN 'outer-caught';
  END;
END; $$;
SELECT cov_exc_rethrow();

CREATE FUNCTION cov_exc_reraise() RETURNS text LANGUAGE uplpgsql AS $$
BEGIN
  BEGIN
    BEGIN RAISE EXCEPTION 'inner';
    EXCEPTION WHEN others THEN RAISE; END;
  EXCEPTION WHEN others THEN RETURN 'reraised';
  END;
END; $$;
SELECT cov_exc_reraise();

CREATE FUNCTION cov_exc_nested() RETURNS int LANGUAGE uplpgsql AS $$
DECLARE x int := 0;
BEGIN
  BEGIN
    BEGIN RAISE EXCEPTION 'e'; EXCEPTION WHEN others THEN x := 1; END;
    x := x + 1;
  EXCEPTION WHEN others THEN x := 99;
  END;
  RETURN x;
END; $$;
SELECT cov_exc_nested();

-- repeated entry must not accumulate frames (or double-free them)
CREATE FUNCTION cov_exc_loop(n int) RETURNS int LANGUAGE uplpgsql AS $$
DECLARE i int; c int := 0;
BEGIN
  FOR i IN 1..n LOOP
    BEGIN RAISE EXCEPTION 'e'; EXCEPTION WHEN others THEN c := c + 1; END;
  END LOOP;
  RETURN c;
END; $$;
SELECT cov_exc_loop(2000);

CREATE FUNCTION cov_exc_loop_ret(n int) RETURNS text LANGUAGE uplpgsql AS $$
DECLARE i int;
BEGIN
  FOR i IN 1..n LOOP
    BEGIN
      RAISE EXCEPTION 'e';
    EXCEPTION WHEN others THEN
      IF i = 500 THEN RETURN 'exit-at-500'; END IF;
    END;
  END LOOP;
  RETURN 'no';
END; $$;
SELECT cov_exc_loop_ret(2000);

DROP FUNCTION cov_nested_null();
DROP FUNCTION cov_nested_null2();
DROP FUNCTION cov_nested_deep();
DROP FUNCTION cov_nested_ok();
DROP FUNCTION cov_nested_deep_ok();
DROP FUNCTION cov_nested_null_cond();
DROP FUNCTION cov_exc_normal();
DROP FUNCTION cov_exc_caught();
DROP FUNCTION cov_exc_ret_try();
DROP FUNCTION cov_exc_ret_handler();
DROP FUNCTION cov_exc_rethrow();
DROP FUNCTION cov_exc_reraise();
DROP FUNCTION cov_exc_nested();
DROP FUNCTION cov_exc_loop(int);
DROP FUNCTION cov_exc_loop_ret(int);

--
-- Event triggers (interpreter-only dispatch: no JIT path)
--
CREATE FUNCTION cov_evt() RETURNS event_trigger LANGUAGE uplpgsql AS $$
BEGIN
  RAISE NOTICE 'event trigger: % %', TG_EVENT, TG_TAG;
END; $$;

CREATE EVENT TRIGGER cov_evt_start ON ddl_command_start EXECUTE FUNCTION cov_evt();
CREATE TABLE cov_evt_probe(i int);
DROP TABLE cov_evt_probe;
DROP EVENT TRIGGER cov_evt_start;

-- sql_drop, which carries a different event name
CREATE FUNCTION cov_evt_drop() RETURNS event_trigger LANGUAGE uplpgsql AS $$
BEGIN
  RAISE NOTICE 'sql_drop fired: %', TG_EVENT;
END; $$;
CREATE EVENT TRIGGER cov_evt_sqldrop ON sql_drop EXECUTE FUNCTION cov_evt_drop();
CREATE TABLE cov_evt_probe2(i int);
DROP TABLE cov_evt_probe2;
DROP EVENT TRIGGER cov_evt_sqldrop;

-- cleanup
DROP FUNCTION cov_rowcount();
DROP FUNCTION cov_rowcount_upd();
DROP FUNCTION cov_rowcount_zero();
DROP FUNCTION cov_pg_context();
DROP FUNCTION cov_stacked();
DROP FUNCTION cov_stacked_div();
DROP FUNCTION cov_table(int);
DROP FUNCTION cov_table_query(int);
DROP FUNCTION cov_setof();
DROP TYPE cov_pair;
DROP FUNCTION cov_return_query_exec();
DROP FUNCTION cov_int4_min_div();
DROP FUNCTION cov_int4_min_mod();
DROP FUNCTION cov_int8_min_div();
DROP FUNCTION cov_int8_min_mod();
DROP FUNCTION cov_int4_neg_min();
DROP FUNCTION cov_int4_add_ovf();
DROP FUNCTION cov_int4_mul_ovf();
DROP FUNCTION cov_int4_div_zero();
DROP FUNCTION cov_int4_mod_zero();
DROP FUNCTION cov_shadow_warn();
DROP FUNCTION cov_noshadow();
DROP FUNCTION cov_strict_multi();
DROP FUNCTION cov_strict_multi_few();
DROP FUNCTION cov_lax_multi();
DROP FUNCTION cov_evt();
DROP FUNCTION cov_evt_drop();
DROP TABLE cov_gd;

--
-- NULL through arithmetic and comparison.
--
-- Every operator Tier 1 compiles is strict, so a NULL operand makes the whole
-- expression NULL.  Reading the variable's datum without consulting its isnull
-- flag silently treats NULL as zero.
--
create function nul_add() returns text language uplpgsql as $$
declare a int; b int := 1; r int; begin r := a + b; return coalesce(r::text,'NULL'); end; $$;
select nul_add();
create function nul_add_rev() returns text language uplpgsql as $$
declare a int := 1; b int; r int; begin r := a + b; return coalesce(r::text,'NULL'); end; $$;
select nul_add_rev();
create function nul_sub() returns text language uplpgsql as $$
declare a int; b int := 1; r int; begin r := a - b; return coalesce(r::text,'NULL'); end; $$;
select nul_sub();
create function nul_mul() returns text language uplpgsql as $$
declare a int; b int := 2; r int; begin r := a * b; return coalesce(r::text,'NULL'); end; $$;
select nul_mul();
create function nul_div() returns text language uplpgsql as $$
declare a int; b int := 2; r int; begin r := a / b; return coalesce(r::text,'NULL'); end; $$;
select nul_div();
create function nul_mod() returns text language uplpgsql as $$
declare a int; b int := 2; r int; begin r := a % b; return coalesce(r::text,'NULL'); end; $$;
select nul_mod();
create function nul_neg() returns text language uplpgsql as $$
declare a int; r int; begin r := -a; return coalesce(r::text,'NULL'); end; $$;
select nul_neg();
create function nul_both() returns text language uplpgsql as $$
declare a int; b int; r int; begin r := a + b; return coalesce(r::text,'NULL'); end; $$;
select nul_both();
create function nul_int8() returns text language uplpgsql as $$
declare a bigint; b bigint := 1; r bigint; begin r := a + b; return coalesce(r::text,'NULL'); end; $$;
select nul_int8();
create function nul_float8() returns text language uplpgsql as $$
declare a float8; b float8 := 1; r float8; begin r := a + b; return coalesce(r::text,'NULL'); end; $$;
select nul_float8();
-- a NULL operand must not reach the division: PostgreSQL returns NULL rather
-- than raising division_by_zero, because the strict operator is never invoked
create function nul_div_zero() returns text language uplpgsql as $$
declare a int; b int := 0; r int;
begin r := a / b; return coalesce(r::text,'NULL');
exception when division_by_zero then return 'div0'; end; $$;
select nul_div_zero();
-- non-NULL arithmetic must be unaffected
create function nul_ok() returns text language uplpgsql as $$
declare a int := 7; b int := 3; r int; begin r := a * b + 1; return r::text; end; $$;
select nul_ok();

-- comparisons: a NULL operand makes the condition NULL, which is not true
create function nul_lt() returns text language uplpgsql as $$
declare a int; b int := 1; begin if a < b then return 'true'; else return 'false-or-null'; end if; end; $$;
select nul_lt();
create function nul_eq() returns text language uplpgsql as $$
declare a int; b int := 1; begin if a = b then return 'true'; else return 'false-or-null'; end if; end; $$;
select nul_eq();
create function nul_gt() returns text language uplpgsql as $$
declare a int; b int := 1; begin if a > b then return 'true'; else return 'false-or-null'; end if; end; $$;
select nul_gt();
create function nul_while() returns int language uplpgsql as $$
declare a int; n int := 0; begin while a < 5 loop n := n + 1; exit when n > 2; end loop; return n; end; $$;
select nul_while();
create function nul_exit_when() returns int language uplpgsql as $$
declare a int; n int := 0; begin loop n := n + 1; exit when a > 0; exit when n > 4; end loop; return n; end; $$;
select nul_exit_when();

--
-- Three-valued logic: AND/OR/NOT are the only non-strict operators.
--
create function tvl_and_true() returns text language uplpgsql as $$
declare a bool; r bool; begin r := a and true; return coalesce(r::text,'NULL'); end; $$;
select tvl_and_true();
create function tvl_and_false() returns text language uplpgsql as $$
declare a bool; b bool := false; r bool; begin r := a and b; return coalesce(r::text,'NULL'); end; $$;
select tvl_and_false();
create function tvl_or_true() returns text language uplpgsql as $$
declare a bool; b bool := true; r bool; begin r := a or b; return coalesce(r::text,'NULL'); end; $$;
select tvl_or_true();
create function tvl_or_false() returns text language uplpgsql as $$
declare a bool; b bool := false; r bool; begin r := a or b; return coalesce(r::text,'NULL'); end; $$;
select tvl_or_false();
create function tvl_not_null() returns text language uplpgsql as $$
declare a bool; r bool; begin r := not a; return coalesce(r::text,'NULL'); end; $$;
select tvl_not_null();
create function tvl_and_null() returns text language uplpgsql as $$
declare a bool; b bool; r bool; begin r := a and b; return coalesce(r::text,'NULL'); end; $$;
select tvl_and_null();
create function tvl_3arg_and() returns text language uplpgsql as $$
declare a bool := true; b bool; c bool := false; r bool; begin r := a and b and c; return coalesce(r::text,'NULL'); end; $$;
select tvl_3arg_and();
create function tvl_3arg_or() returns text language uplpgsql as $$
declare a bool := false; b bool; c bool := true; r bool; begin r := a or b or c; return coalesce(r::text,'NULL'); end; $$;
select tvl_3arg_or();
-- ordinary two-valued cases must still work
create function tvl_plain() returns text language uplpgsql as $$
declare a bool := true; b bool := false; begin return (a and b)::text || '/' || (a or b)::text || '/' || (not a)::text; end; $$;
select tvl_plain();
-- IF treats a NULL condition as not-true
create function tvl_if_not_null() returns text language uplpgsql as $$
declare a bool; begin if not a then return 'T'; else return 'F-or-N'; end if; end; $$;
select tvl_if_not_null();
create function tvl_if_or_true() returns text language uplpgsql as $$
declare a bool; b bool := true; begin if a or b then return 'T'; else return 'F-or-N'; end if; end; $$;
select tvl_if_or_true();
create function tvl_if_and_true() returns text language uplpgsql as $$
declare a bool; b bool := true; begin if a and b then return 'T'; else return 'F-or-N'; end if; end; $$;
select tvl_if_and_true();

--
-- float8 semantics: PostgreSQL's operators raise where IEEE would saturate,
-- and order NaN above every other value.
--
create function flt_div_zero() returns text language uplpgsql as $$
declare a float8 := 1; b float8 := 0; r float8;
begin r := a / b; return r::text;
exception when division_by_zero then return 'div0'; end; $$;
select flt_div_zero();
create function flt_overflow() returns text language uplpgsql as $$
declare a float8 := 1e308; b float8 := 10; r float8;
begin r := a * b; return r::text;
exception when others then return sqlstate; end; $$;
select flt_overflow();
create function flt_underflow() returns text language uplpgsql as $$
declare a float8 := 1e-320; b float8 := 1e10; r float8;
begin r := a / b; return r::text;
exception when others then return sqlstate; end; $$;
select flt_underflow();
create function flt_nan_gt() returns text language uplpgsql as $$
declare a float8 := 'NaN'; b float8 := 1; begin if a > b then return 'gt'; else return 'not-gt'; end if; end; $$;
select flt_nan_gt();
create function flt_gt_nan() returns text language uplpgsql as $$
declare a float8 := 1; b float8 := 'NaN'; begin if a > b then return 'gt'; else return 'not-gt'; end if; end; $$;
select flt_gt_nan();
create function flt_nan_eq() returns text language uplpgsql as $$
declare a float8 := 'NaN'; b float8 := 'NaN'; begin if a = b then return 'eq'; else return 'ne'; end if; end; $$;
select flt_nan_eq();
create function flt_nan_ne() returns text language uplpgsql as $$
declare a float8 := 'NaN'; b float8 := 1; begin if a <> b then return 'ne'; else return 'eq'; end if; end; $$;
select flt_nan_ne();
create function flt_inf() returns text language uplpgsql as $$
declare a float8 := 'Infinity'; b float8 := 1; r float8; begin r := a + b; return r::text; end; $$;
select flt_inf();
create function flt_normal() returns text language uplpgsql as $$
declare a float8 := 1.5; b float8 := 2; r float8; begin r := a * b + 1; return r::text; end; $$;
select flt_normal();
create function flt_neg_zero() returns text language uplpgsql as $$
declare a float8 := -0.0; b float8 := 0.0; begin if a = b then return 'eq'; else return 'ne'; end if; end; $$;
select flt_neg_zero();

drop function nul_add(); drop function nul_add_rev(); drop function nul_sub();
drop function nul_mul(); drop function nul_div(); drop function nul_mod();
drop function nul_neg(); drop function nul_both(); drop function nul_int8();
drop function nul_float8(); drop function nul_div_zero(); drop function nul_ok();
drop function nul_lt(); drop function nul_eq(); drop function nul_gt();
drop function nul_while(); drop function nul_exit_when();
drop function tvl_and_true(); drop function tvl_and_false(); drop function tvl_or_true();
drop function tvl_or_false(); drop function tvl_not_null(); drop function tvl_and_null();
drop function tvl_3arg_and(); drop function tvl_3arg_or(); drop function tvl_plain();
drop function tvl_if_not_null(); drop function tvl_if_or_true(); drop function tvl_if_and_true();
drop function flt_div_zero(); drop function flt_overflow(); drop function flt_underflow();
drop function flt_nan_gt(); drop function flt_gt_nan(); drop function flt_nan_eq();
drop function flt_nan_ne(); drop function flt_inf(); drop function flt_normal();
drop function flt_neg_zero();

--
-- float8 with PostgreSQL semantics, compiled natively.
--
-- These are the cases a raw IEEE lowering gets wrong: the guards in
-- float8_pl/_mul/_div mean Infinity and zero operands must NOT raise, while
-- a finite computation that becomes Infinity or zero must.  NaN sorts above
-- everything, and NaN = NaN is true.
--

-- division by zero raises; but NaN/0 does not (the check is
-- "val2 == 0 && !isnan(val1)")
create function fn_div0() returns text language uplpgsql as $$
declare a float8 := 1; b float8 := 0; r float8;
begin r := a / b; return r::text;
exception when division_by_zero then return 'div0'; end; $$;
select fn_div0();
create function fn_zero_div0() returns text language uplpgsql as $$
declare a float8 := 0; b float8 := 0; r float8;
begin r := a / b; return r::text;
exception when division_by_zero then return 'div0'; end; $$;
select fn_zero_div0();
create function fn_nan_div0() returns text language uplpgsql as $$
declare a float8 := 'NaN'; b float8 := 0; r float8;
begin r := a / b; return r::text;
exception when division_by_zero then return 'div0'; end; $$;
select fn_nan_div0();

-- a finite computation reaching Infinity overflows
create function fn_ovf_mul() returns text language uplpgsql as $$
declare a float8 := 1e308; b float8 := 10; r float8;
begin r := a * b; return r::text; exception when others then return sqlstate; end; $$;
select fn_ovf_mul();
create function fn_ovf_add() returns text language uplpgsql as $$
declare a float8 := 1.7e308; b float8 := 1.7e308; r float8;
begin r := a + b; return r::text; exception when others then return sqlstate; end; $$;
select fn_ovf_add();

-- but an Infinity operand does not: the result was already infinite
create function fn_inf_add() returns text language uplpgsql as $$
declare a float8 := 'Infinity'; b float8 := 1; r float8; begin r := a + b; return r::text; end; $$;
select fn_inf_add();
create function fn_inf_mul() returns text language uplpgsql as $$
declare a float8 := 'Infinity'; b float8 := 2; r float8; begin r := a * b; return r::text; end; $$;
select fn_inf_mul();
create function fn_inf_div() returns text language uplpgsql as $$
declare a float8 := 'Infinity'; b float8 := 2; r float8; begin r := a / b; return r::text; end; $$;
select fn_inf_div();

-- a finite computation collapsing to zero underflows
create function fn_unf_div() returns text language uplpgsql as $$
declare a float8 := 1e-320; b float8 := 1e10; r float8;
begin r := a / b; return r::text; exception when others then return sqlstate; end; $$;
select fn_unf_div();
create function fn_unf_mul() returns text language uplpgsql as $$
declare a float8 := 1e-320; b float8 := 1e-10; r float8;
begin r := a * b; return r::text; exception when others then return sqlstate; end; $$;
select fn_unf_mul();

-- but a zero operand does not, nor does dividing by Infinity
create function fn_zero_mul() returns text language uplpgsql as $$
declare a float8 := 0; b float8 := 5; r float8; begin r := a * b; return r::text; end; $$;
select fn_zero_mul();
create function fn_div_inf() returns text language uplpgsql as $$
declare a float8 := 1; b float8 := 'Infinity'; r float8; begin r := a / b; return r::text; end; $$;
select fn_div_inf();

-- NaN propagates without raising
create function fn_nan_add() returns text language uplpgsql as $$
declare a float8 := 'NaN'; b float8 := 1; r float8; begin r := a + b; return r::text; end; $$;
select fn_nan_add();

-- ordinary arithmetic and unary minus
create function fn_normal() returns text language uplpgsql as $$
declare a float8 := 1.5; b float8 := 2; r float8; begin r := a * b + 1; return r::text; end; $$;
select fn_normal();
create function fn_neg() returns text language uplpgsql as $$
declare a float8 := 2.5; r float8; begin r := -a; return r::text; end; $$;
select fn_neg();

-- the full NaN comparison matrix: NaN is greater than everything, equal to
-- itself.  LLVM's ordered predicates alone answer false to all of these.
create function fn_cmp() returns text language uplpgsql as $$
declare n float8 := 'NaN'; o float8 := 1; s text := '';
begin
  s := s || (n =  o)::text || ',' || (o =  n)::text || ',' || (n =  n)::text || '/';
  s := s || (n <> o)::text || ',' || (o <> n)::text || ',' || (n <> n)::text || '/';
  s := s || (n <  o)::text || ',' || (o <  n)::text || ',' || (n <  n)::text || '/';
  s := s || (n <= o)::text || ',' || (o <= n)::text || ',' || (n <= n)::text || '/';
  s := s || (n >  o)::text || ',' || (o >  n)::text || ',' || (n >  n)::text || '/';
  s := s || (n >= o)::text || ',' || (o >= n)::text || ',' || (n >= n)::text;
  return s;
end; $$;
select fn_cmp();

-- NaN comparison driving control flow (the Tier 1 bool path)
create function fn_cmp_if() returns text language uplpgsql as $$
declare n float8 := 'NaN'; o float8 := 1;
begin if n > o then return 'nan-is-greater'; else return 'no'; end if; end; $$;
select fn_cmp_if();

-- NULL still propagates through the native float path: the strict guard must
-- skip the computation, so neither the zero divide nor the overflow fires
create function fn_null_add() returns text language uplpgsql as $$
declare a float8; b float8 := 1; r float8; begin r := a + b; return coalesce(r::text,'NULL'); end; $$;
select fn_null_add();
create function fn_null_div0() returns text language uplpgsql as $$
declare a float8; b float8 := 0; r float8;
begin r := a / b; return coalesce(r::text,'NULL');
exception when division_by_zero then return 'div0'; end; $$;
select fn_null_div0();
create function fn_null_ovf() returns text language uplpgsql as $$
declare a float8; b float8 := 1e308; r float8;
begin r := a * b; return coalesce(r::text,'NULL');
exception when others then return sqlstate; end; $$;
select fn_null_ovf();
create function fn_null_cmp() returns text language uplpgsql as $$
declare a float8; b float8 := 1; begin if a < b then return 'T'; else return 'F-or-N'; end if; end; $$;
select fn_null_cmp();

-- float8 arrays exercise the native subscript read feeding native arithmetic
create function fn_array_sum() returns text language uplpgsql as $$
declare x float8[]; t float8 := 0; i int;
begin
  x := array_fill(1.5::float8, array[4]);
  for i in 1..4 loop t := t + x[i] * 2.0; end loop;
  return t::text;
end; $$;
select fn_array_sum();

drop function fn_div0(); drop function fn_zero_div0(); drop function fn_nan_div0();
drop function fn_ovf_mul(); drop function fn_ovf_add();
drop function fn_inf_add(); drop function fn_inf_mul(); drop function fn_inf_div();
drop function fn_unf_div(); drop function fn_unf_mul();
drop function fn_zero_mul(); drop function fn_div_inf(); drop function fn_nan_add();
drop function fn_normal(); drop function fn_neg(); drop function fn_cmp();
drop function fn_cmp_if(); drop function fn_null_add(); drop function fn_null_div0();
drop function fn_null_ovf(); drop function fn_null_cmp(); drop function fn_array_sum();
