--
-- Benchmark workloads: PL/pgSQL interpreter compared with the uplpgsql JIT.
--
-- Each body is written once and installed under both languages, so the two
-- engines execute byte-identical source and it cannot drift.  That is the
-- whole methodological requirement, and it needs nothing but format():
--
--   EXECUTE format('CREATE FUNCTION w_i4_%s(...) AS %L LANGUAGE %s', lang, body, lang)
--
-- Every workload is checked for an identical return value under both engines
-- before any timing is reported.  A benchmark that has not proved the two
-- engines agree is measuring nothing in particular.
--
-- Usage:
--   psql -d mydb -f bench/workloads.sql
--   bench/run.sh
--

DO $install$
DECLARE
	lang	text;
	w		record;
BEGIN
	FOR w IN
		SELECT * FROM (VALUES

		-- int4 scalar arithmetic
		('i4', 'n int', 'int', $body$
DECLARE s int := 0; i int := 1;
BEGIN
  WHILE i <= n LOOP s := (s + i) % 100000; i := i + 1; END LOOP;
  RETURN s;
END $body$),

		-- int8 scalar arithmetic
		('i8', 'n bigint', 'bigint', $body$
DECLARE s bigint := 0; i bigint := 1;
BEGIN
  WHILE i <= n LOOP s := (s + i*i) % 1000000007; i := i + 1; END LOOP;
  RETURN s;
END $body$),

		-- numeric arithmetic.
		-- This is the original "float8" workload from the plx benchmark,
		-- unchanged.  An unadorned 1.0 is numeric in PostgreSQL, so
		-- 1.0/(i*i) is numeric division with an int8 operand and the loop
		-- never touches float8 at all -- the float8 is only s, which the
		-- numeric result is cast into on assignment.  Numeric has no native
		-- path, which is what the modest speedup here reflects.
		('num', 'n bigint', 'float8', $body$
DECLARE s float8 := 0; i bigint := 1;
BEGIN
  WHILE i <= n LOOP s := s + 1.0/(i*i); i := i + 1; END LOOP;
  RETURN s;
END $body$),

		-- float8 arithmetic, for real this time
		('f8', 'n bigint', 'float8', $body$
DECLARE s float8 := 0; i bigint := 1;
BEGIN
  WHILE i <= n LOOP s := s + 1.0::float8/(i*i)::float8; i := i + 1; END LOOP;
  RETURN s;
END $body$),

		-- branch and nested loop: total Collatz step count for 1..n
		('coll', 'n bigint', 'bigint', $body$
DECLARE total bigint := 0; i bigint := 1; x bigint;
BEGIN
  WHILE i <= n LOOP
    x := i;
    WHILE x > 1 LOOP
      IF x % 2 = 0 THEN x := x / 2; ELSE x := 3*x + 1; END IF;
      total := total + 1;
    END LOOP;
    i := i + 1;
  END LOOP;
  RETURN total;
END $body$),

		-- nested loop: fixed-point Mandelbrot escape-time checksum
		('mand', 'dim int', 'bigint', $body$
DECLARE px int; py int; it int; zr bigint; zi bigint; zr2 bigint; zi2 bigint;
        cr bigint; ci bigint; nr bigint; sum bigint := 0;
BEGIN
  py := 0;
  WHILE py < dim LOOP
    ci := (-2*65536) + ((4*65536)*py) / (dim-1);
    px := 0;
    WHILE px < dim LOOP
      cr := (-2*65536) + ((4*65536)*px) / (dim-1);
      zr := 0; zi := 0; it := 0;
      WHILE it < 100 LOOP
        zr2 := (zr*zr) / 65536; zi2 := (zi*zi) / 65536;
        IF zr2 + zi2 > 4*65536 THEN EXIT; END IF;
        nr := zr2 - zi2 + cr;
        zi := ((2*zr*zi) / 65536) + ci;
        zr := nr; it := it + 1;
      END LOOP;
      sum := (sum + it*px + py) % 2147483647;
      px := px + 1;
    END LOOP;
    py := py + 1;
  END LOOP;
  RETURN sum;
END $body$),

		-- array element reads: the shape a COBOL OCCURS table compiles to
		('arr', 'n int', 'bigint', $body$
DECLARE t int[]; s bigint := 0; i int; k int;
BEGIN
  t := array_fill(0, ARRAY[64]);
  FOR i IN 1..64 LOOP t[i] := i*7; END LOOP;
  FOR i IN 1..n LOOP
    k := (i % 64) + 1;
    s := s + t[k];
  END LOOP;
  RETURN s;
END $body$),

		-- array element writes, value needing a cast to the element type
		('arrw', 'n int', 'bigint', $body$
DECLARE t int[]; s bigint := 0; i int; v float8 := 2.0;
BEGIN
  t := array_fill(0, ARRAY[n]);
  FOR i IN 1..n LOOP t[i] := floor(power(v, 0.45)*255.0 + 0.5)::int; END LOOP;
  FOR i IN 1..n LOOP s := s + t[i]; END LOOP;
  RETURN s;
END $body$)

		) AS t(nm, args, ret, body)
	LOOP
		FOREACH lang IN ARRAY ARRAY['plpgsql', 'uplpgsql'] LOOP
			EXECUTE format('CREATE OR REPLACE FUNCTION w_%s_%s(%s) RETURNS %s '
						   'AS %L LANGUAGE %s',
						   w.nm, lang, w.args, w.ret, w.body, lang);
		END LOOP;
	END LOOP;
END
$install$;
