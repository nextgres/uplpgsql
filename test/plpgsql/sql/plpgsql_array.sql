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
