-- Simple test to verify accessibility of the OLD and NEW trigger variables

create table testtr (a int, b text);

create function testtr_trigger() returns trigger LANGUAGE uplpgsql as
$$begin
  raise notice 'tg_op = %', tg_op;
  raise notice 'old(%) = %', old.a, row(old.*);
  raise notice 'new(%) = %', new.a, row(new.*);
  if (tg_op = 'DELETE') then
    return old;
  else
    return new;
  end if;
end$$;

create trigger testtr_trigger before insert or delete or update on testtr
  for each row execute function testtr_trigger();

insert into testtr values (1, 'one'), (2, 'two');

update testtr set a = a + 1;

delete from testtr;

--
-- Trigger variants beyond BEFORE-ROW: AFTER, statement-level, WHEN(...),
-- INSTEAD OF, transition tables, and constraint triggers.
--
create table trg_v_t(i int, v text);

create function trg_v_fn() returns trigger language uplpgsql as $$
begin
  raise notice '% % % nargs=% arg0=%', tg_when, tg_level, tg_op, tg_nargs, tg_argv[0];
  if tg_level = 'ROW' and tg_op = 'DELETE' then return old; end if;
  if tg_level = 'ROW' then return new; end if;
  return null;
end; $$;

create trigger trg_v_after_row after insert or update or delete on trg_v_t
  for each row execute function trg_v_fn('after-row');
create trigger trg_v_stmt after insert on trg_v_t
  for each statement execute function trg_v_fn('stmt');

insert into trg_v_t values (1,'a');
update trg_v_t set v = 'b';
delete from trg_v_t;

-- WHEN(...): fires only when the condition holds
create trigger trg_v_when before update on trg_v_t
  for each row when (old.i is distinct from new.i) execute function trg_v_fn('when');
insert into trg_v_t values (1,'a');
-- i unchanged: WHEN trigger must not fire
update trg_v_t set v = 'c';
-- i changed: WHEN trigger fires
update trg_v_t set i = 2;
drop trigger trg_v_when on trg_v_t;

-- INSTEAD OF on a view
create view trg_v_view as select * from trg_v_t;
create function trg_v_io() returns trigger language uplpgsql as $$
begin
  raise notice 'instead of % on view', tg_op;
  return new;
end; $$;
create trigger trg_v_io_t instead of insert on trg_v_view
  for each row execute function trg_v_io();
insert into trg_v_view values (9,'z');

-- transition tables
create function trg_v_tt() returns trigger language uplpgsql as $$
declare n int;
begin
  select count(*) into n from newtab;
  raise notice 'transition rows=%', n;
  return null;
end; $$;
create trigger trg_v_tt_t after insert on trg_v_t
  referencing new table as newtab for each statement execute function trg_v_tt();
insert into trg_v_t values (5,'x'),(6,'y');

-- constraint trigger
create function trg_v_c() returns trigger language uplpgsql as $$
begin
  raise notice 'constraint trigger: %', tg_op;
  return null;
end; $$;
create constraint trigger trg_v_c_t after insert on trg_v_t
  deferrable for each row execute function trg_v_c();
insert into trg_v_t values (7,'q');

drop view trg_v_view;
drop table trg_v_t cascade;
drop function trg_v_fn();
drop function trg_v_io();
drop function trg_v_tt();
drop function trg_v_c();
