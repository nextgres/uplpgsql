-- Benchmark workloads for the PL/pgSQL interpreter vs uplpgsql JIT comparison.
--
-- Written in the plx "plsql" dialect (https://github.com/commandprompt/plx).
-- plx transpiles these to PL/pgSQL at CREATE FUNCTION time and stores the
-- result in pg_proc.prosrc, which both engines then execute.
--
-- Requires: CREATE EXTENSION plx; CREATE EXTENSION uplpgsql;

-- int8 scalar arithmetic
CREATE OR REPLACE FUNCTION w_i8(n bigint) RETURNS bigint LANGUAGE plxplsql AS $$
DECLARE s bigint := 0; i bigint := 1;
BEGIN
  WHILE i <= n LOOP s := (s + i*i) % 1000000007; i := i + 1; END LOOP;
  RETURN s;
END $$;

-- int4 scalar arithmetic
CREATE OR REPLACE FUNCTION w_i4(n int) RETURNS int LANGUAGE plxplsql AS $$
DECLARE s int := 0; i int := 1;
BEGIN
  WHILE i <= n LOOP s := (s + i) % 100000; i := i + 1; END LOOP;
  RETURN s;
END $$;

-- float8 arithmetic
CREATE OR REPLACE FUNCTION w_f8(n bigint) RETURNS float8 LANGUAGE plxplsql AS $$
DECLARE s float8 := 0; i bigint := 1;
BEGIN
  WHILE i <= n LOOP s := s + 1.0/(i*i); i := i + 1; END LOOP;
  RETURN s;
END $$;

-- branch and nested loop: total Collatz step count for 1..n
CREATE OR REPLACE FUNCTION w_coll(n bigint) RETURNS bigint LANGUAGE plxplsql AS $$
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
END $$;

-- nested loop: fixed-point Mandelbrot escape-time checksum over a dim x dim grid
CREATE OR REPLACE FUNCTION w_mand(dim int) RETURNS bigint LANGUAGE plxplsql AS $$
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
END $$;
