--
-- Tests for PL/pgSQL handling of array variables
--
-- We also check arrays of composites here, so this has some overlap
-- with the plpgsql_record tests.
--

create type complex as (r float8, i float8);
create type quadarray as (c1 complex[], c2 complex);

do LANGUAGE uplpgsql $$ declare a int[];
begin a := array[1,2]; a[3] := 4; raise notice 'a = %', a; end$$;

do LANGUAGE uplpgsql $$ declare a int[];
begin a[3] := 4; raise notice 'a = %', a; end$$;

do LANGUAGE uplpgsql $$ declare a int[];
begin a[1][4] := 4; raise notice 'a = %', a; end$$;

do LANGUAGE uplpgsql $$ declare a int[];
begin a[1] := 23::text; raise notice 'a = %', a; end$$;  -- lax typing

do LANGUAGE uplpgsql $$ declare a int[];
begin a := array[1,2]; a[2:3] := array[3,4]; raise notice 'a = %', a; end$$;

do LANGUAGE uplpgsql $$ declare a int[];
begin a := array[1,2]; a[2] := a[2] + 1; raise notice 'a = %', a; end$$;

do LANGUAGE uplpgsql $$ declare a int[];
begin a[1:2] := array[3,4]; raise notice 'a = %', a; end$$;

do LANGUAGE uplpgsql $$ declare a int[];
begin a[1:2] := 4; raise notice 'a = %', a; end$$;  -- error

do LANGUAGE uplpgsql $$ declare a complex[];
begin a[1] := (1,2); a[1].i := 11; raise notice 'a = %', a; end$$;

do LANGUAGE uplpgsql $$ declare a complex[];
begin a[1].i := 11; raise notice 'a = %, a[1].i = %', a, a[1].i; end$$;

-- perhaps this ought to work, but for now it doesn't:
do LANGUAGE uplpgsql $$ declare a complex[];
begin a[1:2].i := array[11,12]; raise notice 'a = %', a; end$$;

do LANGUAGE uplpgsql $$ declare a quadarray;
begin a.c1[1].i := 11; raise notice 'a = %, a.c1[1].i = %', a, a.c1[1].i; end$$;

do LANGUAGE uplpgsql $$ declare a int[];
begin a := array_agg(x) from (values(1),(2),(3)) v(x); raise notice 'a = %', a; end$$;

do LANGUAGE uplpgsql $$ declare a int[] := array[1,2,3];
begin
  -- test scenarios for optimization of updates of R/W expanded objects
  a := array_append(a, 42);  -- optimizable using "transfer" method
  a := a || a[3];  -- optimizable using "inplace" method
  a := a[1] || a;  -- ditto, but let's test array_prepend
  a := a || a;     -- not optimizable
  raise notice 'a = %', a;
end$$;

create temp table onecol as select array[1,2] as f1;

do LANGUAGE uplpgsql $$ declare a int[];
begin a := f1 from onecol; raise notice 'a = %', a; end$$;

do LANGUAGE uplpgsql $$ declare a int[];
begin a := * from onecol for update; raise notice 'a = %', a; end$$;

-- error cases:

do LANGUAGE uplpgsql $$ declare a int[];
begin a := from onecol; raise notice 'a = %', a; end$$;

do LANGUAGE uplpgsql $$ declare a int[];
begin a := f1, f1 from onecol; raise notice 'a = %', a; end$$;

insert into onecol values(array[11]);

do LANGUAGE uplpgsql $$ declare a int[];
begin a := f1 from onecol; raise notice 'a = %', a; end$$;

do LANGUAGE uplpgsql $$ declare a int[];
begin a := f1 from onecol limit 1; raise notice 'a = %', a; end$$;

do LANGUAGE uplpgsql $$ declare a real;
begin a[1] := 2; raise notice 'a = %', a; end$$;

do LANGUAGE uplpgsql $$ declare a complex;
begin a.r[1] := 2; raise notice 'a = %', a; end$$;

--
-- test of %type[] and %rowtype[] syntax
--

-- check supported syntax
do LANGUAGE uplpgsql $$
declare
  v int;
  v1 v%type;
  v2 v%type[];
  v3 v%type[1];
  v4 v%type[][];
  v5 v%type[1][3];
  v6 v%type array;
  v7 v%type array[];
  v8 v%type array[1];
  v9 v%type array[1][1];
  v10 pg_catalog.pg_class%rowtype[];
begin
  raise notice '%', pg_typeof(v1);
  raise notice '%', pg_typeof(v2);
  raise notice '%', pg_typeof(v3);
  raise notice '%', pg_typeof(v4);
  raise notice '%', pg_typeof(v5);
  raise notice '%', pg_typeof(v6);
  raise notice '%', pg_typeof(v7);
  raise notice '%', pg_typeof(v8);
  raise notice '%', pg_typeof(v9);
  raise notice '%', pg_typeof(v10);
end;
$$;

-- some types don't support arrays
do LANGUAGE uplpgsql $$
declare
  v pg_node_tree;
  v1 v%type[];
begin
end;
$$;

-- check functionality
do LANGUAGE uplpgsql $$
declare
  v1 int;
  v2 varchar;
  a1 v1%type[];
  a2 v2%type[];
begin
  v1 := 10;
  v2 := 'Hi';
  a1 := array[v1,v1];
  a2 := array[v2,v2];
  raise notice '% %', a1, a2;
end;
$$;

create table array_test_table(a int, b varchar);

insert into array_test_table values(1, 'first'), (2, 'second');

do LANGUAGE uplpgsql $$
declare tg array_test_table%rowtype[];
begin
  tg := array(select array_test_table from array_test_table);
  raise notice '%', tg;
  tg := array(select row(a,b) from array_test_table);
  raise notice '%', tg;
end;
$$;

--
-- Native array escape analysis.
--
-- Local int/float arrays are held in flat native memory (data_ptr/len_ptr)
-- rather than as PG array Datums.  Any operation that reads or writes the
-- variable as a whole Datum must marshal between the two representations,
-- or the two views drift apart and silently return stale data.
--

-- whole-datum read: y := x must see x's live contents, not its stale Datum
create or replace function na_read_escape() returns int language uplpgsql as $$
declare x int[]; y int[];
begin
  x := array_fill(7, array[5]);
  y := x;
  return y[1];
end; $$;
select na_read_escape();

-- whole-datum write: x := array[...] must not leave flat memory stale
create or replace function na_write_escape() returns int language uplpgsql as $$
declare x int[];
begin
  x := array_fill(7, array[5]);
  x := array[1,2,3];
  return x[1];
end; $$;
select na_write_escape();

-- whole-datum read by a function call, then subscript reads afterwards
create or replace function na_read_then_subscript() returns text language uplpgsql as $$
declare x int[]; s text;
begin
  x := array_fill(7, array[3]);
  s := array_to_string(x, ',');
  x[2] := 9;
  return s || '/' || x[2]::text;
end; $$;
select na_read_then_subscript();

-- round trip: write whole, read by subscript, write by subscript, read whole
create or replace function na_round_trip() returns text language uplpgsql as $$
declare x int[];
begin
  x := array_fill(0, array[3]);
  x := array[10,20,30];
  x[2] := x[1] + x[3];
  return array_to_string(x, ',');
end; $$;
select na_round_trip();

-- whole-datum escapes through statements that delegate to the interpreter:
-- RAISE, FOREACH and EXECUTE all read the variable as a PG Datum
create or replace function na_raise_escape() returns text language uplpgsql as $$
declare x int[];
begin
  x := array_fill(7, array[3]);
  raise notice 'na_raise_escape: %', x;
  return 'ok';
end; $$;
select na_raise_escape();

create or replace function na_foreach_escape() returns int language uplpgsql as $$
declare x int[]; e int; t int := 0;
begin
  x := array_fill(7, array[3]);
  foreach e in array x loop t := t + e; end loop;
  return t;
end; $$;
select na_foreach_escape();

create or replace function na_execute_escape() returns int language uplpgsql as $$
declare x int[]; r int;
begin
  x := array_fill(7, array[3]);
  execute 'select ($1)[1]' into r using x;
  return r;
end; $$;
select na_execute_escape();

-- The native form is a flat 1-D vector with lower bound 1, and marshalling
-- back out rebuilds with construct_array().  Arrays that cannot round-trip
-- through that -- multi-dimensional, or a non-standard lower bound -- must be
-- refused by the native path, or a sync silently rewrites the variable as a
-- flattened 1-D array.
create or replace function na_multidim_dims() returns text language uplpgsql as $$
declare a int[] := array[[1,2],[3,4]];
begin
  return array_dims(a) || '/' || array_ndims(a) || '/' || a[1][2];
end; $$;
select na_multidim_dims();

create or replace function na_multidim_foreach() returns text language uplpgsql as $$
declare a int[] := array[[1,2],[3,4]]; s int[]; t int := 0; n int := 0;
begin
  foreach s slice 1 in array a loop n := n + 1; t := t + s[1]; end loop;
  return 'iters=' || n || ' sum=' || t;
end; $$;
select na_multidim_foreach();

create or replace function na_multidim_fill() returns text language uplpgsql as $$
declare a int[];
begin
  a := array_fill(7, array[2,2]);
  return array_dims(a);
end; $$;
select na_multidim_fill();

create or replace function na_multidim_into() returns text language uplpgsql as $$
declare a int[];
begin
  select array[[1,2],[3,4]] into a;
  return array_dims(a);
end; $$;
select na_multidim_into();

-- a lower bound other than 1 also cannot round-trip
create or replace function na_lowerbound() returns text language uplpgsql as $$
declare a int[] := '[2:3]={9,10}'::int[];
begin
  return array_dims(a) || '/' || a[2];
end; $$;
select na_lowerbound();

drop function na_multidim_dims();
drop function na_multidim_foreach();
drop function na_multidim_fill();
drop function na_multidim_into();
drop function na_lowerbound();
-- loop cursors open through uplpgsql_call_fn, not uplpgsql_call_exec, so they
-- do not inherit that function's sync: each open must sync explicitly or the
-- query sees a stale NULL array and matches no rows.
create or replace function na_fors_escape() returns int language uplpgsql as $$
declare x int[]; t int := 0; rec record;
begin
  x := array_fill(7, array[3]);
  for rec in select unnest(x) as v loop t := t + rec.v; end loop;
  return t;
end; $$;
select na_fors_escape();

create or replace function na_dynfors_escape() returns int language uplpgsql as $$
declare x int[]; t int := 0; rec record;
begin
  x := array_fill(7, array[3]);
  for rec in execute 'select unnest($1) as v' using x loop t := t + rec.v; end loop;
  return t;
end; $$;
select na_dynfors_escape();

create or replace function na_forc_escape() returns int language uplpgsql as $$
declare x int[]; t int := 0; rec record; c cursor for select unnest(x) as v;
begin
  x := array_fill(7, array[3]);
  for rec in c loop t := t + rec.v; end loop;
  return t;
end; $$;
select na_forc_escape();

drop function na_fors_escape();
drop function na_dynfors_escape();
drop function na_forc_escape();
--
-- PostgreSQL array semantics on the native path.
--
-- A native array is flat memory, but PostgreSQL arrays are not fixed-size
-- vectors: a subscript outside the bounds reads as NULL rather than raising,
-- an assignment outside the bounds *extends* the array (filling the gap with
-- NULLs), the lower bound need not be 1, and a single subscript on a
-- multi-dimensional value is NULL.  The native path has to reproduce all of
-- it, so the fast in-bounds store is guarded and everything else defers to
-- array_set_element.
--

-- out-of-bounds reads are NULL, not an error
create or replace function na_oob_read() returns text language uplpgsql as $$
declare a int[]; begin
  a := array_fill(1, array[3]);
  return coalesce(a[10]::text,'N') || '/' || coalesce(a[0]::text,'N') || '/'
         || coalesce(a[-1]::text,'N');
end; $$;
select na_oob_read();

create or replace function na_null_read() returns text language uplpgsql as $$
declare a int[]; r int; begin r := a[1]; return coalesce(r::text,'NULL'); end; $$;
select na_null_read();

-- an out-of-bounds write extends, leaving NULLs in the gap
create or replace function na_oob_write() returns text language uplpgsql as $$
declare a int[]; begin
  a := array_fill(1, array[3]);
  a[6] := 9;
  return array_dims(a) || ' ' || a[6] || ' gap=' || coalesce(a[4]::text,'NULL')
         || ' kept=' || a[2];
end; $$;
select na_oob_write();

-- filling a gap clears that element's NULL flag
create or replace function na_fill_gap() returns text language uplpgsql as $$
declare a int[]; begin
  a := array_fill(1, array[3]);
  a[6] := 9;
  a[4] := 4;
  return a[4]::text || '/' || a[6]::text;
end; $$;
select na_fill_gap();

-- an array grown only by subscript assignment, never sized up front
create or replace function na_grow() returns text language uplpgsql as $$
declare a int[]; i int; s bigint := 0; begin
  for i in 1..5 loop a[i] := i*i; end loop;
  for i in 1..5 loop s := s + a[i]; end loop;
  return s::text || ' ' || array_dims(a);
end; $$;
select na_grow();

create or replace function na_grow_null_default() returns text language uplpgsql as $$
declare a int[] := NULL; begin a[1] := 7; return a[1]::text; end; $$;
select na_grow_null_default();

-- writing to a NULL array bases it at the subscript used
create or replace function na_write_null_arr() returns text language uplpgsql as $$
declare a int[]; begin a[3] := 7; return array_dims(a) || ' ' || a[3]; end; $$;
select na_write_null_arr();

-- a lower bound other than 1 is honoured on read, and on extend
create or replace function na_lb_read() returns text language uplpgsql as $$
declare a int[] := '[2:3]={9,10}'::int[]; begin
  return coalesce(a[2]::text,'N') || '/' || coalesce(a[1]::text,'N');
end; $$;
select na_lb_read();

create or replace function na_lb_extend() returns text language uplpgsql as $$
declare a int[] := '[2:3]={9,10}'::int[]; begin
  a[5] := 1;
  return array_dims(a) || '/' || a[5];
end; $$;
select na_lb_extend();

-- a single subscript on a multi-dimensional value is NULL, and the value
-- itself must survive the round trip unflattened
create or replace function na_2d_single() returns text language uplpgsql as $$
declare a int[] := array[[1,2],[3,4]]; r int; begin
  r := a[1];
  return coalesce(r::text,'NULL') || '/' || array_dims(a) || '/' || a[1][2];
end; $$;
select na_2d_single();

-- int8 and float8 elements take the same paths
create or replace function na_int8_grow() returns text language uplpgsql as $$
declare a bigint[]; begin
  a := array_fill(5::bigint, array[3]);
  a[5] := 7;
  return a[5]::text || '/' || coalesce(a[4]::text,'N');
end; $$;
select na_int8_grow();

create or replace function na_float8_grow() returns text language uplpgsql as $$
declare a float8[]; begin
  a := array_fill(1.5::float8, array[2]);
  a[4] := 2.5;
  return a[4]::text || '/' || coalesce(a[3]::text,'N');
end; $$;
select na_float8_grow();

-- the fast in-bounds path itself
create or replace function na_fast_path() returns text language uplpgsql as $$
declare x int[]; t bigint := 0; i int; begin
  x := array_fill(2, array[100]);
  x[50] := 3;
  for i in 1..100 loop t := t + x[i]; end loop;
  return t::text;
end; $$;
select na_fast_path();

drop function na_oob_read();
drop function na_null_read();
drop function na_oob_write();
drop function na_fill_gap();
drop function na_grow();
drop function na_grow_null_default();
drop function na_write_null_arr();
drop function na_lb_read();
drop function na_lb_extend();
drop function na_2d_single();
drop function na_int8_grow();
drop function na_float8_grow();
drop function na_fast_path();

-- a native array initialised by a DECLARE default.  init_var evaluates the
-- default into the variable's PG Datum; flat memory must be reloaded from it,
-- or native subscripts bounds-check against a length of zero.
create or replace function na_declare_default() returns int language uplpgsql as $$
declare a int[] := array[5,6]; r int;
begin
  r := a[1];
  return r;
end; $$;
select na_declare_default();

create or replace function na_declare_default_write() returns int language uplpgsql as $$
declare a int[] := array[5,6];
begin
  a[1] := 3;
  return a[1];
end; $$;
select na_declare_default_write();

-- same, in a block with exception handlers (separate init path)
create or replace function na_declare_default_exc() returns int language uplpgsql as $$
declare r int;
begin
  declare a int[] := array[8,9];
  begin
    r := a[1];
  exception when others then r := -1;
  end;
  return r;
end; $$;
select na_declare_default_exc();

drop function na_declare_default();
drop function na_declare_default_write();
drop function na_declare_default_exc();
-- a polymorphic built-in (array_length(anyarray,int)) nested as an argument.
-- uplpgsql_can_fmgr_compile() approves the outer call, but fmgr compilation
-- refuses the polymorphic inner one; the resulting NULL must abort the fmgr
-- attempt and fall back, not be emitted as an operand.
create or replace function na_poly_nested() returns int language uplpgsql as $$
declare a int[] := array[5,6]; r int;
begin
  r := array_length(a,1) + 1;
  return r;
end; $$;
select na_poly_nested();

create or replace function na_poly_deep() returns int language uplpgsql as $$
declare a int[] := array[5,6]; r int;
begin
  r := abs(array_length(a,1) * 2) + 1;
  return r;
end; $$;
select na_poly_deep();

drop function na_poly_nested();
drop function na_poly_deep();
-- a native array read by a condition that cannot compile natively falls back
-- to the interpreter (RT_EVAL_BOOL); it must see live data, and compiling the
-- condition must not crash on an argument the fmgr path cannot handle
create or replace function na_if_cond() returns text language uplpgsql as $$
declare x int[];
begin
  x := array_fill(7, array[3]);
  if array_length(x,1) = 3 then return 'yes'; else return 'no'; end if;
end; $$;
select na_if_cond();

create or replace function na_while_cond() returns int language uplpgsql as $$
declare x int[]; n int := 0;
begin
  x := array_fill(7, array[3]);
  while array_length(x,1) > n loop n := n + 1; end loop;
  return n;
end; $$;
select na_while_cond();

drop function na_if_cond();
drop function na_while_cond();
drop function na_raise_escape();
drop function na_foreach_escape();
drop function na_execute_escape();
drop function na_read_escape();
drop function na_write_escape();
drop function na_read_then_subscript();
drop function na_round_trip();

--
-- Values written into a native array element.
--
-- Two things are being pinned here.
--
-- A NULL value must land as a NULL.  The flat store has nowhere to put one --
-- there may be no null flags allocated at all -- so a NULL has to divert to
-- array_set_element rather than write the Datum and clear the element's null
-- flag, which silently turned "a[2] := <null>" into a[2] = 0.
--
-- And a value that needs a Tier 2 call, notably a cast down to the element
-- type, must still take the flat store.  When it did not, the statement went
-- to the interpreter, and because the target is a native array the assign path
-- then reloaded every element from the Datum afterwards: O(n) per element
-- write, so filling an array was quadratic in its length.
--
create function na_store_null() returns text language uplpgsql as $$
declare a int[] := ARRAY[1,2,3]; b int; f float8[] := ARRAY[1,2,3]::float8[]; g float8;
begin
  a[2] := b;
  f[2] := g;
  return a::text || ' ' || f::text;
end; $$;
select na_store_null();

-- a NULL write must not disturb its neighbours, and the slot must be
-- readable as NULL afterwards
create function na_store_null_read() returns text language uplpgsql as $$
declare a int[] := ARRAY[1,2,3,4]; b int; r text;
begin
  a[2] := b;
  a[4] := 9;
  if a[2] is null then r := 'null'; else r := a[2]::text; end if;
  return r || ' ' || a::text;
end; $$;
select na_store_null_read();

-- NULL into a gap left by an extend
create function na_store_null_extend() returns text language uplpgsql as $$
declare a int[] := ARRAY[1,2]; b int;
begin
  a[5] := b;
  a[4] := 4;
  return a::text;
end; $$;
select na_store_null_extend();

-- cast to the element type: int4 element fed from float8 arithmetic
create function na_store_cast(n int) returns int[] language uplpgsql as $$
declare a int[]; i int; v float8 := 2.0;
begin
  a := array_fill(0, ARRAY[n]);
  for i in 1..n loop a[i] := floor(power(v, 0.45)*255.0 + 0.5)::int; end loop;
  return a;
end; $$;
select na_store_cast(4);

-- int8 element from a float8 expression, and a NULL through the same path
create function na_store_cast_int8() returns bigint[] language uplpgsql as $$
declare a bigint[] := ARRAY[0,0,0]::bigint[]; v float8 := 3.7; g float8;
begin
  a[1] := round(v)::bigint;
  a[2] := g::bigint;
  a[3] := (v*2)::bigint;
  return a;
end; $$;
select na_store_cast_int8();

-- the cast store must respect bounds too: out of range still extends
create function na_store_cast_oob() returns int[] language uplpgsql as $$
declare a int[] := ARRAY[1,2]; v float8 := 7.0;
begin
  a[5] := floor(v)::int;
  return a;
end; $$;
select na_store_cast_oob();

drop function na_store_null();
drop function na_store_null_read();
drop function na_store_null_extend();
drop function na_store_cast(int);
drop function na_store_cast_int8();
drop function na_store_cast_oob();

--
-- A statement that falls back to the interpreter marshals only the native
-- arrays it reads.
--
-- Flat memory is the live copy, so anything the interpreter reads has to be
-- marshalled out to its Datum first.  Doing that for every native array in
-- the function regardless is correct but costs a full copy of each, per
-- execution, for data the callee never looks at -- enough to make a JIT'd
-- function slower than the interpreter.  These pin the correctness half: an
-- expression that reaches the interpreter must still see live data for the
-- arrays it does read, while other arrays are mid-flight with stale Datums.
--
-- greatest()/least() are the lever: a MinMaxExpr reaches neither tier, so
-- these assignments genuinely fall back.
--
create function na_selective_sync() returns text language uplpgsql as $$
declare a int[] := ARRAY[1,2,3]; b int[] := ARRAY[9,9,9]; r int;
begin
  a[1] := 42;                    -- native writes: both Datums now stale
  b[1] := 77;
  r := greatest(a[1], 0);        -- falls back, reads a but not b
  return r::text || ' ' || a::text || ' ' || b::text;
end; $$;
select na_selective_sync();

-- the fallback reads the *other* array, after both have been written
create function na_selective_sync2() returns text language uplpgsql as $$
declare a int[] := ARRAY[1,2,3]; b int[] := ARRAY[9,9,9]; r int;
begin
  a[2] := 5;
  b[2] := 6;
  r := least(b[2], 100);         -- falls back, reads b but not a
  return r::text || ' ' || a::text || ' ' || b::text;
end; $$;
select na_selective_sync2();

-- a fallback reading both, and one reading neither, interleaved with writes
create function na_selective_sync3() returns text language uplpgsql as $$
declare a int[] := ARRAY[1,2]; b int[] := ARRAY[3,4]; x int := 7; r int; q int;
begin
  a[1] := 10;
  b[1] := 20;
  q := greatest(x, 0);           -- falls back, reads no array at all
  r := greatest(a[1], b[1]);     -- falls back, reads both
  a[2] := 11;                    -- keep writing after the round trips
  return q::text || ' ' || r::text || ' ' || a::text || ' ' || b::text;
end; $$;
select na_selective_sync3();

-- whole-datum read of an array in a fallback expression
create function na_selective_whole() returns text language uplpgsql as $$
declare a int[] := ARRAY[1,2,3]; b int[] := ARRAY[9,9,9]; c int[];
begin
  a[3] := 30;
  b[3] := 90;
  c := a || b;                   -- falls back, reads both whole arrays
  return c::text;
end; $$;
select na_selective_whole();

drop function na_selective_sync();
drop function na_selective_sync2();
drop function na_selective_sync3();
drop function na_selective_whole();

--
-- Selective marshal must not trust an expression that was never prepared.
--
-- The read-set comes from paramnos, which parse analysis fills in.  The JIT
-- compiles before the function has ever executed, so a fallback ASSIGN whose
-- expression was not prepared at compile time (a record-field target bails
-- out before preparing; a prepare can also fail mid-analysis) has an empty
-- or partial paramnos, and trusting it would bake a stale-array read into
-- the compiled code.  Such statements must marshal every native array.
--

-- record-field target: the fallback's expression is never prepared
create function na_sync_recfield() returns int language uplpgsql as $$
declare a int[]; r record;
begin
  a := array_fill(0, ARRAY[3]);
  select 0 as x into r;
  a[1] := 42;                    -- native flat write; a's Datum is stale
  r.x := a[1];                   -- fallback assign, unprepared expression
  return r.x;
end; $$;
select na_sync_recfield();

-- prepare fails mid-analysis on the record field, leaving paramnos partial
create function na_sync_partial() returns int language uplpgsql as $$
declare a int[]; r record; x int;
begin
  a := array_fill(0, ARRAY[3]);
  select 5 as f into r;
  a[1] := 42;                    -- native flat write; a's Datum is stale
  x := r.f + a[1];               -- r.f cannot be analyzed at compile time
  return x;
end; $$;
select na_sync_partial();

drop function na_sync_recfield();
drop function na_sync_partial();

--
-- Array element NULL semantics (element writes, subscripts, non-native
-- parameter arrays).  Consolidated from PRs #40, #39, #42.
--

-- a NULL leaf in the value of an element write stores a NULL element; every
-- Tier 1 operator is strict, so the operator is never invoked and cannot
-- raise on the NULL path
create function na_nullleaf_div() returns text language uplpgsql as $$
declare a int[] := array[1,2,3]; y int;
begin
  a[1] := 1 / y;
  return a::text;
end; $$;
select na_nullleaf_div();

create function na_nullleaf_ovf() returns text language uplpgsql as $$
declare a int[] := array[1,2,3]; x int;
begin
  a[2] := x + 2147483647;
  return a::text;
end; $$;
select na_nullleaf_ovf();

-- float8 path: z / 0.0 with z NULL is NULL, not division_by_zero
create function na_nullleaf_f8() returns text language uplpgsql as $$
declare a float8[] := array[1.5,2.5]; z float8;
begin
  a[1] := z / 0.0;
  return a::text;
end; $$;
select na_nullleaf_f8();

-- a plain NULL store still works
create function na_nullleaf_plain() returns text language uplpgsql as $$
declare a int[] := array[1,2,3]; y int;
begin
  a[2] := y;
  return a::text;
end; $$;
select na_nullleaf_plain();

-- and a NULL value can extend the array: the gap and the target are NULL
create function na_nullleaf_extend() returns text language uplpgsql as $$
declare a int[] := array[1,2,3]; y int;
begin
  a[6] := 1 / y;
  return a::text || ' len ' || array_length(a, 1)::text;
end; $$;
select na_nullleaf_extend();

-- the standard (non-native) write path: an array parameter is never taken
-- native, and used to store 0 for a NULL value with isnull hardwired false
create function na_nullleaf_std(a int[]) returns text language uplpgsql as $$
declare y int;
begin
  a[2] := y;
  a[3] := 1 / y;
  return a::text;
end; $$;
select na_nullleaf_std(array[1,2,3]);

drop function na_nullleaf_div();
drop function na_nullleaf_ovf();
drop function na_nullleaf_f8();
drop function na_nullleaf_plain();
drop function na_nullleaf_extend();
drop function na_nullleaf_std(int[]);

-- a NULL subscript in an element assignment raises; it must not be consumed
-- as a garbage index (it silently wrote a[0])
create function na_nullsub_var() returns int[] language uplpgsql as $$
declare a int[] := array[1,2,3]; i int;
begin
  a[i] := 99;
  return a;
end; $$;
select na_nullsub_var();

-- the subscript may itself be a native array read: out of range it reads as
-- NULL, and the assignment must then raise rather than fetch a garbage index
-- from past the end of the flat buffer
create function na_nullsub_oob() returns int[] language uplpgsql as $$
declare a int[] := array[1,2,3]; b int[] := array[2,7];
begin
  a[b[9]] := 99;
  return a;
end; $$;
select na_nullsub_oob();

-- same with an in-range subscript element that is NULL
create function na_nullsub_elem() returns int[] language uplpgsql as $$
declare a int[] := array[1,2,3]; b int[] := array[2,null];
begin
  a[b[2]] := 99;
  return a;
end; $$;
select na_nullsub_elem();

-- the fmgr-bypass value arm takes the same subscript check
create function na_nullsub_fmgr() returns int[] language uplpgsql as $$
declare a int[] := array[1,2,3]; i int;
begin
  a[i] := floor(1.5)::int;
  return a;
end; $$;
select na_nullsub_fmgr();

-- the standard (non-native) write path: an array parameter is never native
create function na_nullsub_std(a int[]) returns int[] language uplpgsql as $$
declare i int;
begin
  a[i] := 5;
  return a;
end; $$;
select na_nullsub_std(array[1,2,3]);

-- an in-range variable subscript still works
create function na_nullsub_ok() returns int[] language uplpgsql as $$
declare a int[] := array[1,2,3]; i int := 2;
begin
  a[i] := 99;
  return a;
end; $$;
select na_nullsub_ok();

-- These read the standard (non-native) array element path.  Kept on int[]
-- rather than the PR's numeric[]: a non-native array whose element type is
-- pass-by-reference (numeric, text) crashes array_get_element on an
-- assert-enabled build regardless of NULL handling -- a separate, pre-existing
-- bug tracked on its own.  The isnull fix under test is identical for int[].

-- a NULL subscript in a *read* is not an error: the element reads as NULL
create function na_nullread_std(p int[]) returns text language uplpgsql as $$
declare i int; x int;
begin
  x := p[i];
  return coalesce(x::text, 'NULL');
end; $$;
select na_nullread_std(array[10,20]);

-- and an out-of-range read reports NULL through isNull_out; storing the
-- datum with isnull hardwired false turned it into 0
create function na_nullread_oob(p int[]) returns text language uplpgsql as $$
declare x int;
begin
  x := p[9];
  return coalesce(x::text, 'NULL');
end; $$;
select na_nullread_oob(array[10,20]);

create function na_nullread_ok(p int[]) returns text language uplpgsql as $$
declare x int;
begin
  x := p[2];
  return coalesce(x::text, 'NULL');
end; $$;
select na_nullread_ok(array[10,20]);

drop function na_nullsub_var();
drop function na_nullsub_oob();
drop function na_nullsub_elem();
drop function na_nullsub_fmgr();
drop function na_nullsub_std(int[]);
drop function na_nullsub_ok();
drop function na_nullread_std(numeric[]);
drop function na_nullread_oob(numeric[]);
drop function na_nullread_ok(numeric[]);

--
-- NULL handling on non-native arrays.
--
-- Function-parameter arrays are never taken native (parameters may alias
-- caller data), so their element reads and writes go through the
-- array_get_element/array_set_element runtime helpers.  Those used to drop
-- NULL in both directions: reads ignored the helper's isNull output, so a
-- NULL element, an out-of-range subscript, and a NULL subscript all read as
-- 0; writes passed a hardcoded valisnull = false, so a NULL value was
-- stored as 0.  And a NULL subscript in an assignment must raise, as it
-- does in PostgreSQL.
--

-- read of a NULL element must yield NULL, not 0
create function pa_read(a int[]) returns int language uplpgsql as $$
declare x int;
begin
  x := a[2];
  return x;
end; $$;
select pa_read(ARRAY[1,NULL,3]) is null as read_null;
select pa_read(ARRAY[1,7,3]);

-- out-of-range read must yield NULL, not 0
select pa_read(ARRAY[1]) is null as read_oob;

-- NULL subscript read must yield NULL
create function pa_read_nullsub(a int[]) returns int language uplpgsql as $$
declare i int; x int;
begin
  x := a[i];
  return x;
end; $$;
select pa_read_nullsub(ARRAY[1,2,3]) is null as read_nullsub;

-- a NULL element reaching Tier 1 arithmetic must make the result NULL
create function pa_read_expr(a int[]) returns int language uplpgsql as $$
declare x int;
begin
  x := a[2] + 1;
  return x;
end; $$;
select pa_read_expr(ARRAY[1,NULL,3]) is null as expr_null;
select pa_read_expr(ARRAY[1,7,3]);

-- write of a NULL value must store NULL, not 0
create function pa_write(a int[]) returns int[] language uplpgsql as $$
declare b int;
begin
  a[2] := b;
  return a;
end; $$;
select pa_write(ARRAY[1,2,3]);

-- NULL written then read back, both through the JIT'd paths
create function pa_round_trip(a int[]) returns int language uplpgsql as $$
declare b int; x int;
begin
  a[2] := b;
  x := a[2];
  return x;
end; $$;
select pa_round_trip(ARRAY[1,2,3]) is null as round_trip_null;

-- a NULL value must not be computed: the arithmetic would raise on the
-- garbage datum a NULL variable holds, where PostgreSQL stores a NULL
create function pa_write_expr(a int[], d int) returns int[] language uplpgsql as $$
begin
  a[2] := 10 / d;
  return a;
end; $$;
select pa_write_expr(ARRAY[1,2,3], NULL);
select pa_write_expr(ARRAY[1,2,3], 5);

-- a NULL subscript in an assignment is an error, as in PostgreSQL
create function pa_write_nullsub(a int[]) returns text language uplpgsql as $$
declare i int; msg text;
begin
  begin
    a[i] := 5;
    msg := 'not reached';
  exception when null_value_not_allowed then
    msg := 'caught: ' || sqlerrm;
  end;
  return msg;
end; $$;
select pa_write_nullsub(ARRAY[1,2,3]);

drop function pa_read(int[]);
drop function pa_read_nullsub(int[]);
drop function pa_read_expr(int[]);
drop function pa_write(int[]);
drop function pa_round_trip(int[]);
drop function pa_write_expr(int[], int);
drop function pa_write_nullsub(int[]);
