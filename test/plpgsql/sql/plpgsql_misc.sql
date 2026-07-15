--
-- Miscellaneous topics
--

-- Verify that we can parse new-style CREATE FUNCTION/PROCEDURE
do LANGUAGE uplpgsql $$
  declare procedure int;  -- check we still recognize non-keywords as vars
  begin
  create function test1() returns int
    begin atomic
      select 2 + 2;
    end;
  create or replace procedure test2(x int)
    begin atomic
      select x + 2;
    end;
  end
$$;

\sf test1
\sf test2

-- Test %TYPE and %ROWTYPE error cases
create table misc_table(f1 int);

do LANGUAGE uplpgsql $$ declare x foo%type; begin end $$;
do LANGUAGE uplpgsql $$ declare x notice%type; begin end $$;  -- covers unreserved-keyword case
do LANGUAGE uplpgsql $$ declare x foo.bar%type; begin end $$;
do LANGUAGE uplpgsql $$ declare x foo.bar.baz%type; begin end $$;
do LANGUAGE uplpgsql $$ declare x public.foo.bar%type; begin end $$;
do LANGUAGE uplpgsql $$ declare x public.misc_table.zed%type; begin end $$;

do LANGUAGE uplpgsql $$ declare x foo%rowtype; begin end $$;
do LANGUAGE uplpgsql $$ declare x notice%rowtype; begin end $$;  -- covers unreserved-keyword case
do LANGUAGE uplpgsql $$ declare x foo.bar%rowtype; begin end $$;
do LANGUAGE uplpgsql $$ declare x foo.bar.baz%rowtype; begin end $$;
do LANGUAGE uplpgsql $$ declare x public.foo%rowtype; begin end $$;
do LANGUAGE uplpgsql $$ declare x public.misc_table%rowtype; begin end $$;

-- Test handling of an unreserved keyword as a variable name
-- and record field name.
do LANGUAGE uplpgsql $$
declare
  execute int;
  r record;
begin
  execute := 10;
  raise notice 'execute = %', execute;
  select 1 as strict into r;
  raise notice 'r.strict = %', r.strict;
end $$;

-- Test handling of a reserved keyword as a record field name.

do LANGUAGE uplpgsql $$ declare r record;
begin
  select 1 as x, 2 as foreach into r;
  raise notice 'r.x = %', r.x;
  raise notice 'r.foreach = %', r.foreach;  -- fails
end $$;

do LANGUAGE uplpgsql $$ declare r record;
begin
  select 1 as x, 2 as foreach into r;
  raise notice 'r.x = %', r.x;
  raise notice 'r."foreach" = %', r."foreach";  -- ok
end $$;
