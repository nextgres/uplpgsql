--
-- Tests for uplpgsql JIT-specific compilation paths
--
-- These tests exercise edge cases in LLVM IR generation, expression tiers,
-- cache invalidation, and compilation fallback that the adapted PL/pgSQL
-- regression tests don't adequately cover.
--

-------------------------------------------------------------------
-- 6a. Tier 1 arithmetic edge cases
-------------------------------------------------------------------

-- Integer overflow detection (int4)
CREATE FUNCTION jit_int4_add_overflow() RETURNS void AS $$
DECLARE v int4 := 2147483647;
BEGIN
  v := v + 1;  -- should raise overflow
END
$$ LANGUAGE uplpgsql;

SELECT jit_int4_add_overflow();

CREATE FUNCTION jit_int4_sub_underflow() RETURNS void AS $$
DECLARE v int4 := -2147483648;
BEGIN
  v := v - 1;  -- should raise overflow
END
$$ LANGUAGE uplpgsql;

SELECT jit_int4_sub_underflow();

CREATE FUNCTION jit_int4_mul_overflow() RETURNS void AS $$
DECLARE v int4 := 2147483647;
BEGIN
  v := v * 2;  -- should raise overflow
END
$$ LANGUAGE uplpgsql;

SELECT jit_int4_mul_overflow();

-- Integer overflow detection (int8)
CREATE FUNCTION jit_int8_add_overflow() RETURNS void AS $$
DECLARE v int8 := 9223372036854775807;
BEGIN
  v := v + 1;  -- should raise overflow
END
$$ LANGUAGE uplpgsql;

SELECT jit_int8_add_overflow();

CREATE FUNCTION jit_int8_sub_underflow() RETURNS void AS $$
DECLARE v int8 := -9223372036854775808;
BEGIN
  v := v - 1;  -- should raise overflow
END
$$ LANGUAGE uplpgsql;

SELECT jit_int8_sub_underflow();

-- Division by zero (int4 and int8)
CREATE FUNCTION jit_int4_div_zero() RETURNS void AS $$
DECLARE v int4 := 42; d int4 := 0;
BEGIN
  v := v / d;
END
$$ LANGUAGE uplpgsql;

SELECT jit_int4_div_zero();

CREATE FUNCTION jit_int4_mod_zero() RETURNS void AS $$
DECLARE v int4 := 42; d int4 := 0;
BEGIN
  v := v % d;
END
$$ LANGUAGE uplpgsql;

SELECT jit_int4_mod_zero();

CREATE FUNCTION jit_int8_div_zero() RETURNS void AS $$
DECLARE v int8 := 42; d int8 := 0;
BEGIN
  v := v / d;
END
$$ LANGUAGE uplpgsql;

SELECT jit_int8_div_zero();

-- MIN_INT / -1 (int4 and int8 — undefined behavior in C, must raise error)
CREATE FUNCTION jit_int4_min_div_neg1() RETURNS void AS $$
DECLARE v int4 := -2147483648; d int4 := -1;
BEGIN
  v := v / d;
END
$$ LANGUAGE uplpgsql;

SELECT jit_int4_min_div_neg1();

CREATE FUNCTION jit_int8_min_div_neg1() RETURNS void AS $$
DECLARE v int8 := -9223372036854775808; d int8 := -1;
BEGIN
  v := v / d;
END
$$ LANGUAGE uplpgsql;

SELECT jit_int8_min_div_neg1();

-- Unary negation overflow (negating MIN_INT)
CREATE FUNCTION jit_int4_neg_overflow() RETURNS void AS $$
DECLARE v int4 := -2147483648;
BEGIN
  v := -v;  -- should raise overflow
END
$$ LANGUAGE uplpgsql;

SELECT jit_int4_neg_overflow();

CREATE FUNCTION jit_int8_neg_overflow() RETURNS void AS $$
DECLARE v int8 := -9223372036854775808;
BEGIN
  v := -v;  -- should raise overflow
END
$$ LANGUAGE uplpgsql;

SELECT jit_int8_neg_overflow();

-- Cross-type int4/int8 arithmetic
CREATE FUNCTION jit_cross_type_arith() RETURNS text AS $$
DECLARE
  a int4 := 100;
  b int8 := 200000000000;
  r int8;
  r2 int8;
BEGIN
  r := a + b;   -- int4 widened to int8 for addition
  r2 := a * b;  -- int4 widened to int8 for multiplication
  RETURN 'add=' || r::text || ' mul=' || r2::text;
END
$$ LANGUAGE uplpgsql;

SELECT jit_cross_type_arith();

-- Float8 operations
CREATE FUNCTION jit_float8_ops() RETURNS text AS $$
DECLARE
  a float8 := 3.14;
  b float8 := 2.0;
  r text := '';
BEGIN
  r := r || 'add=' || (a + b)::text;
  r := r || ' sub=' || (a - b)::text;
  r := r || ' mul=' || (a * b)::text;
  r := r || ' div=' || (a / b)::text;
  r := r || ' neg=' || (-a)::text;
  RETURN r;
END
$$ LANGUAGE uplpgsql;

SELECT jit_float8_ops();

-- Normal arithmetic that should just work (verify correctness)
CREATE FUNCTION jit_arith_correctness() RETURNS text AS $$
DECLARE
  a int4 := 100;
  b int4 := 7;
  r text := '';
BEGIN
  r := r || (a + b)::text;       -- 107
  r := r || ',' || (a - b)::text; -- 93
  r := r || ',' || (a * b)::text; -- 700
  r := r || ',' || (a / b)::text; -- 14
  r := r || ',' || (a % b)::text; -- 2
  RETURN r;
END
$$ LANGUAGE uplpgsql;

SELECT jit_arith_correctness();

-------------------------------------------------------------------
-- 6b. Tier 2 fmgr bypass edge cases
-------------------------------------------------------------------

-- Strict function with NULL args (NULL propagation via PHI merge)
CREATE FUNCTION jit_strict_null() RETURNS text AS $$
DECLARE
  a text := 'hello';
  b text := NULL;
  r text;
BEGIN
  r := a || b;  -- textcat is strict, NULL arg → NULL result
  RETURN coalesce(r, '<NULL>');
END
$$ LANGUAGE uplpgsql;

SELECT jit_strict_null();

-- Pass-by-ref types in Tier 2 (text operations)
CREATE FUNCTION jit_text_concat_loop() RETURNS text AS $$
DECLARE
  r text := '';
  i int;
BEGIN
  FOR i IN 1..5 LOOP
    r := r || i::text;
  END LOOP;
  RETURN r;
END
$$ LANGUAGE uplpgsql;

SELECT jit_text_concat_loop();

-- Polymorphic function falls back to Tier 3 correctly
CREATE FUNCTION jit_polymorphic_concat() RETURNS text AS $$
DECLARE
  v_cnt int := 42;
  v_text text;
BEGIN
  v_text := 'count=' || v_cnt;  -- || with int uses textanycat (polymorphic)
  RETURN v_text;
END
$$ LANGUAGE uplpgsql;

SELECT jit_polymorphic_concat();

-- Mixed tiers in same function
CREATE FUNCTION jit_mixed_tiers() RETURNS text AS $$
DECLARE
  a int4 := 10;
  b int4 := 3;
  c text;
  d int4;
BEGIN
  d := a * b + a / b;         -- Tier 1: native int arithmetic
  c := 'result=' || d::text;  -- Tier 2 or 3: text concat
  RETURN c;
END
$$ LANGUAGE uplpgsql;

SELECT jit_mixed_tiers();

-------------------------------------------------------------------
-- 6c. Compilation fallback
-------------------------------------------------------------------

-- Function that compiles and runs correctly via JIT
CREATE FUNCTION jit_simple_works() RETURNS int AS $$
DECLARE x int := 0;
BEGIN
  FOR i IN 1..10 LOOP
    x := x + i;
  END LOOP;
  RETURN x;  -- 55
END
$$ LANGUAGE uplpgsql;

SELECT jit_simple_works();

-- Expression that can't be compiled (sub-select) falls to Tier 3
CREATE TABLE jit_test_data (id int, val text);
INSERT INTO jit_test_data VALUES (1, 'hello'), (2, 'world');

CREATE FUNCTION jit_subselect_fallback() RETURNS text AS $$
DECLARE r text;
BEGIN
  r := (SELECT val FROM jit_test_data WHERE id = 1);
  RETURN r;
END
$$ LANGUAGE uplpgsql;

SELECT jit_subselect_fallback();

-------------------------------------------------------------------
-- 6d. Cache invalidation
-------------------------------------------------------------------

-- CREATE OR REPLACE triggers recompilation
CREATE FUNCTION jit_cache_replace() RETURNS int AS $$
BEGIN RETURN 1; END
$$ LANGUAGE uplpgsql;

SELECT jit_cache_replace();  -- should return 1

CREATE OR REPLACE FUNCTION jit_cache_replace() RETURNS int AS $$
BEGIN RETURN 2; END
$$ LANGUAGE uplpgsql;

SELECT jit_cache_replace();  -- should return 2 (recompiled)

CREATE OR REPLACE FUNCTION jit_cache_replace() RETURNS int AS $$
BEGIN RETURN 3; END
$$ LANGUAGE uplpgsql;

SELECT jit_cache_replace();  -- should return 3 (recompiled again)

-- Function that errors on first call, succeeds on second
-- (tests function pointer change detection in cache)
CREATE FUNCTION jit_cache_error_recovery(x int) RETURNS int AS $$
BEGIN
  RETURN 10 / x;  -- x=0 errors, x=2 succeeds
END
$$ LANGUAGE uplpgsql;

SELECT jit_cache_error_recovery(0);   -- ERROR: division by zero
SELECT jit_cache_error_recovery(2);   -- should return 5

-------------------------------------------------------------------
-- 6e. Exception blocks
-------------------------------------------------------------------

-- RETURN inside try body (exception_return_bb path)
CREATE FUNCTION jit_exception_return_in_try() RETURNS int AS $$
BEGIN
  BEGIN
    RETURN 42;  -- RETURN inside try must commit subtxn before returning
  EXCEPTION WHEN division_by_zero THEN
    RETURN -1;
  END;
END
$$ LANGUAGE uplpgsql;

SELECT jit_exception_return_in_try();

-- Nested exception blocks
CREATE FUNCTION jit_nested_exceptions() RETURNS text AS $$
DECLARE r text := '';
BEGIN
  BEGIN
    BEGIN
      r := r || 'inner-try ';
      PERFORM 1/0;  -- trigger inner exception
    EXCEPTION WHEN division_by_zero THEN
      r := r || 'inner-catch ';
    END;
    r := r || 'outer-try ';
  EXCEPTION WHEN others THEN
    r := r || 'outer-catch ';
  END;
  RETURN r;
END
$$ LANGUAGE uplpgsql;

SELECT jit_nested_exceptions();

-- Handler variables (SQLSTATE, SQLERRM)
CREATE FUNCTION jit_exception_handler_vars() RETURNS text AS $$
DECLARE
  r text;
BEGIN
  BEGIN
    PERFORM 1/0;
  EXCEPTION WHEN division_by_zero THEN
    r := 'state=' || SQLSTATE || ' msg=' || SQLERRM;
  END;
  RETURN r;
END
$$ LANGUAGE uplpgsql;

SELECT jit_exception_handler_vars();

-- Variables modified in try body visible after exception
CREATE FUNCTION jit_exception_var_visibility() RETURNS text AS $$
DECLARE
  x int := 0;
  r text;
BEGIN
  BEGIN
    x := 10;
    PERFORM 1/0;
    x := 20;  -- never reached
  EXCEPTION WHEN division_by_zero THEN
    -- x is 10: subtransaction rollback does NOT revert in-memory scalar values
    r := 'x=' || x::text;
  END;
  RETURN r;
END
$$ LANGUAGE uplpgsql;

SELECT jit_exception_var_visibility();

-- Multiple exception handlers (switch dispatch)
CREATE FUNCTION jit_multi_handler(mode int) RETURNS text AS $$
BEGIN
  BEGIN
    IF mode = 1 THEN
      PERFORM 1/0;
    ELSIF mode = 2 THEN
      RAISE EXCEPTION 'custom error' USING ERRCODE = 'P0001';
    ELSE
      RETURN 'no error';
    END IF;
  EXCEPTION
    WHEN division_by_zero THEN
      RETURN 'caught div_zero';
    WHEN raise_exception THEN
      RETURN 'caught raise';
  END;
END
$$ LANGUAGE uplpgsql;

SELECT jit_multi_handler(1);
SELECT jit_multi_handler(2);
SELECT jit_multi_handler(3);

-------------------------------------------------------------------
-- 6f. JIT heuristic GUC
-------------------------------------------------------------------

-- With heuristic ON, a no-loop function should be skipped (interpreted)
-- but still produce correct results
CREATE FUNCTION jit_heuristic_noloop(x int) RETURNS int AS $$
BEGIN
  RETURN x * 2 + 1;
END
$$ LANGUAGE uplpgsql;

SET uplpgsql.enable_jit_heuristic = on;
SELECT jit_heuristic_noloop(20);  -- should return 41

-- With heuristic ON, a loop function should be JIT'd
CREATE FUNCTION jit_heuristic_loop(n int) RETURNS int AS $$
DECLARE s int := 0;
BEGIN
  FOR i IN 1..n LOOP
    s := s + i;
  END LOOP;
  RETURN s;
END
$$ LANGUAGE uplpgsql;

SELECT jit_heuristic_loop(10);  -- should return 55

-- Reset to default (JIT everything)
SET uplpgsql.enable_jit_heuristic = off;

-- Verify both functions still work with heuristic off
SELECT jit_heuristic_noloop(20);
SELECT jit_heuristic_loop(10);

-------------------------------------------------------------------
-- 6g. Domain constraints during variable initialization
-------------------------------------------------------------------

CREATE DOMAIN posint AS int CHECK (VALUE > 0);
CREATE DOMAIN nonnull_text AS text NOT NULL;

-- Scalar domain: value constraint violation during init
CREATE FUNCTION jit_domain_scalar_init(x int) RETURNS posint AS $$
DECLARE v posint := x;
BEGIN
  RETURN v;
END
$$ LANGUAGE uplpgsql;

SELECT jit_domain_scalar_init(5);   -- ok
SELECT jit_domain_scalar_init(-1);  -- ERROR: domain constraint

-- Scalar domain: NULL constraint violation during init
CREATE FUNCTION jit_domain_null_init() RETURNS text AS $$
DECLARE v nonnull_text;  -- default NULL should violate NOT NULL
BEGIN
  RETURN v;
END
$$ LANGUAGE uplpgsql;

SELECT jit_domain_null_init();

-- Composite domain constraint during init
CREATE TYPE pair AS (a int, b int);
CREATE DOMAIN ordered_pair AS pair CHECK ((VALUE).a <= (VALUE).b);

CREATE FUNCTION jit_domain_composite_init(x int, y int) RETURNS text AS $$
DECLARE v ordered_pair := ROW(x, y);
BEGIN
  RETURN v::text;
END
$$ LANGUAGE uplpgsql;

SELECT jit_domain_composite_init(1, 2);  -- ok
SELECT jit_domain_composite_init(5, 3);  -- ERROR: domain constraint

-------------------------------------------------------------------
-- 6h. Loop edge cases
-------------------------------------------------------------------

-- FOR integer with empty range
CREATE FUNCTION jit_for_empty_range() RETURNS text AS $$
DECLARE entered bool := false;
BEGIN
  FOR i IN 10..1 LOOP  -- empty range (not REVERSE)
    entered := true;
  END LOOP;
  RETURN 'entered=' || entered::text || ' found=' || FOUND::text;
END
$$ LANGUAGE uplpgsql;

SELECT jit_for_empty_range();

-- EXIT/CONTINUE targeting outer loop labels
CREATE FUNCTION jit_nested_loop_labels() RETURNS text AS $$
DECLARE r text := '';
BEGIN
  <<outer_loop>>
  FOR i IN 1..3 LOOP
    FOR j IN 1..3 LOOP
      IF j = 2 THEN
        CONTINUE outer_loop;  -- skip rest of inner loop AND current outer iteration
      END IF;
      r := r || '(' || i::text || ',' || j::text || ') ';
    END LOOP;
    r := r || 'inner_done ';  -- should never appear (CONTINUE skips it)
  END LOOP;
  RETURN r;
END
$$ LANGUAGE uplpgsql;

SELECT jit_nested_loop_labels();

-- EXIT from outer loop label
CREATE FUNCTION jit_exit_outer_label() RETURNS text AS $$
DECLARE r text := '';
BEGIN
  <<outer_loop>>
  FOR i IN 1..5 LOOP
    FOR j IN 1..5 LOOP
      IF i = 2 AND j = 3 THEN
        EXIT outer_loop;  -- exit both loops
      END IF;
      r := r || i::text || j::text || ' ';
    END LOOP;
  END LOOP;
  RETURN r;
END
$$ LANGUAGE uplpgsql;

SELECT jit_exit_outer_label();

-- WHILE loop with FOUND
CREATE FUNCTION jit_while_found() RETURNS text AS $$
DECLARE
  r text := '';
  cnt int := 0;
BEGIN
  -- FOUND is false initially
  r := r || 'initial=' || FOUND::text;
  WHILE cnt < 3 LOOP
    cnt := cnt + 1;
  END LOOP;
  r := r || ' after_while=' || FOUND::text;
  RETURN r;
END
$$ LANGUAGE uplpgsql;

SELECT jit_while_found();

-------------------------------------------------------------------
-- 6i. FOR-query loops with record field access (Phase 5d)
-------------------------------------------------------------------

-- Setup test table
CREATE TABLE jit_fors_data (a int, b int, c text);
INSERT INTO jit_fors_data SELECT i, i*2, 'row' || i::text FROM generate_series(1,10) i;

-- Basic FOR-SELECT with record field access (inline GEP fast path)
CREATE FUNCTION jit_fors_record_fields() RETURNS text AS $$
DECLARE
  r RECORD;
  sum_a int := 0;
  sum_b int := 0;
BEGIN
  FOR r IN SELECT a, b FROM jit_fors_data ORDER BY a LOOP
    sum_a := sum_a + r.a;
    sum_b := sum_b + r.b;
  END LOOP;
  RETURN 'sum_a=' || sum_a::text || ' sum_b=' || sum_b::text;
END
$$ LANGUAGE uplpgsql;

SELECT jit_fors_record_fields();

-- FOR-SELECT with text field (pass-by-ref record field)
CREATE FUNCTION jit_fors_text_field() RETURNS text AS $$
DECLARE
  r RECORD;
  result text := '';
BEGIN
  FOR r IN SELECT c FROM jit_fors_data ORDER BY a LIMIT 3 LOOP
    result := result || r.c || ',';
  END LOOP;
  RETURN result;
END
$$ LANGUAGE uplpgsql;

SELECT jit_fors_text_field();

-- Nested FOR-SELECT loops (each gets own cursor context)
CREATE FUNCTION jit_fors_nested() RETURNS text AS $$
DECLARE
  r1 RECORD;
  r2 RECORD;
  cnt int := 0;
BEGIN
  FOR r1 IN SELECT a FROM jit_fors_data WHERE a <= 3 ORDER BY a LOOP
    FOR r2 IN SELECT b FROM jit_fors_data WHERE b <= 6 ORDER BY b LOOP
      cnt := cnt + 1;
    END LOOP;
  END LOOP;
  RETURN 'cnt=' || cnt::text;
END
$$ LANGUAGE uplpgsql;

SELECT jit_fors_nested();

-- FOR-SELECT with empty result set
CREATE FUNCTION jit_fors_empty() RETURNS text AS $$
DECLARE
  r RECORD;
  cnt int := 0;
BEGIN
  FOR r IN SELECT a FROM jit_fors_data WHERE a > 100 LOOP
    cnt := cnt + 1;
  END LOOP;
  RETURN 'cnt=' || cnt::text || ' found=' || FOUND::text;
END
$$ LANGUAGE uplpgsql;

SELECT jit_fors_empty();

-- FOR-SELECT with EXIT mid-loop
CREATE FUNCTION jit_fors_exit() RETURNS text AS $$
DECLARE
  r RECORD;
  last_a int := 0;
BEGIN
  FOR r IN SELECT a FROM jit_fors_data ORDER BY a LOOP
    last_a := r.a;
    EXIT WHEN r.a = 5;
  END LOOP;
  RETURN 'last_a=' || last_a::text;
END
$$ LANGUAGE uplpgsql;

SELECT jit_fors_exit();

-------------------------------------------------------------------
-- Cleanup
-------------------------------------------------------------------

DROP FUNCTION jit_int4_add_overflow();
DROP FUNCTION jit_int4_sub_underflow();
DROP FUNCTION jit_int4_mul_overflow();
DROP FUNCTION jit_int8_add_overflow();
DROP FUNCTION jit_int8_sub_underflow();
DROP FUNCTION jit_int4_div_zero();
DROP FUNCTION jit_int4_mod_zero();
DROP FUNCTION jit_int8_div_zero();
DROP FUNCTION jit_int4_min_div_neg1();
DROP FUNCTION jit_int8_min_div_neg1();
DROP FUNCTION jit_int4_neg_overflow();
DROP FUNCTION jit_int8_neg_overflow();
DROP FUNCTION jit_cross_type_arith();
DROP FUNCTION jit_float8_ops();
DROP FUNCTION jit_arith_correctness();
DROP FUNCTION jit_strict_null();
DROP FUNCTION jit_text_concat_loop();
DROP FUNCTION jit_polymorphic_concat();
DROP FUNCTION jit_mixed_tiers();
DROP FUNCTION jit_simple_works();
DROP FUNCTION jit_subselect_fallback();
DROP TABLE jit_test_data;
DROP FUNCTION jit_cache_replace();
DROP FUNCTION jit_cache_error_recovery(int);
DROP FUNCTION jit_exception_return_in_try();
DROP FUNCTION jit_nested_exceptions();
DROP FUNCTION jit_exception_handler_vars();
DROP FUNCTION jit_exception_var_visibility();
DROP FUNCTION jit_multi_handler(int);
DROP FUNCTION jit_heuristic_noloop(int);
DROP FUNCTION jit_heuristic_loop(int);
DROP FUNCTION jit_domain_scalar_init(int);
DROP FUNCTION jit_domain_null_init();
DROP FUNCTION jit_domain_composite_init(int, int);
DROP DOMAIN ordered_pair;
DROP TYPE pair;
DROP DOMAIN posint;
DROP DOMAIN nonnull_text;
DROP FUNCTION jit_for_empty_range();
DROP FUNCTION jit_nested_loop_labels();
DROP FUNCTION jit_exit_outer_label();
DROP FUNCTION jit_while_found();
DROP FUNCTION jit_fors_record_fields();
DROP FUNCTION jit_fors_text_field();
DROP FUNCTION jit_fors_nested();
DROP FUNCTION jit_fors_empty();
DROP FUNCTION jit_fors_exit();
DROP TABLE jit_fors_data;

-------------------------------------------------------------------
-- 6j. Trigger support — promise datums (TG_OP, TG_TABLE_NAME, etc.)
-------------------------------------------------------------------

CREATE TABLE jit_trigger_test(id int, val text);

-- Trigger that uses TG_OP promise datum and modifies NEW
CREATE FUNCTION jit_trigger_tg_op() RETURNS trigger AS $$
BEGIN
  IF TG_OP = 'INSERT' THEN
    NEW.val := NEW.val || '_inserted';
    RETURN NEW;
  ELSIF TG_OP = 'UPDATE' THEN
    NEW.val := NEW.val || '_updated';
    RETURN NEW;
  ELSIF TG_OP = 'DELETE' THEN
    RETURN OLD;
  END IF;
  RETURN NULL;
END
$$ LANGUAGE uplpgsql;

CREATE TRIGGER trg_jit_tg_op
  BEFORE INSERT OR UPDATE OR DELETE ON jit_trigger_test
  FOR EACH ROW EXECUTE FUNCTION jit_trigger_tg_op();

-- Test INSERT
INSERT INTO jit_trigger_test VALUES (1, 'hello');
SELECT * FROM jit_trigger_test;

-- Test UPDATE
UPDATE jit_trigger_test SET val = 'world' WHERE id = 1;
SELECT * FROM jit_trigger_test;

-- Test DELETE
DELETE FROM jit_trigger_test WHERE id = 1;
SELECT * FROM jit_trigger_test;

DROP TRIGGER trg_jit_tg_op ON jit_trigger_test;
DROP FUNCTION jit_trigger_tg_op();

-- Trigger using TG_TABLE_NAME and TG_WHEN promise datums
CREATE FUNCTION jit_trigger_promises() RETURNS trigger AS $$
DECLARE
  info text;
BEGIN
  info := TG_TABLE_NAME || ':' || TG_WHEN || ':' || TG_OP;
  RAISE NOTICE 'trigger info: %', info;
  RETURN NEW;
END
$$ LANGUAGE uplpgsql;

CREATE TRIGGER trg_jit_promises
  BEFORE INSERT ON jit_trigger_test
  FOR EACH ROW EXECUTE FUNCTION jit_trigger_promises();

INSERT INTO jit_trigger_test VALUES (1, 'test');

DROP TRIGGER trg_jit_promises ON jit_trigger_test;
DROP FUNCTION jit_trigger_promises();

-- Statement-level trigger (no NEW/OLD)
CREATE FUNCTION jit_trigger_stmt() RETURNS trigger AS $$
BEGIN
  RAISE NOTICE 'statement trigger: % on %', TG_OP, TG_TABLE_NAME;
  RETURN NULL;
END
$$ LANGUAGE uplpgsql;

CREATE TRIGGER trg_jit_stmt
  AFTER INSERT ON jit_trigger_test
  FOR EACH STATEMENT EXECUTE FUNCTION jit_trigger_stmt();

INSERT INTO jit_trigger_test VALUES (2, 'stmt');

DROP TRIGGER trg_jit_stmt ON jit_trigger_test;
DROP FUNCTION jit_trigger_stmt();

-- Cleanup
DROP TABLE jit_trigger_test;
