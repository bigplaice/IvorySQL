--
-- Tests for procedures / CALL syntax
--

CREATE PROCEDURE test_proc1()
LANGUAGE plisql
AS $$
BEGIN
    NULL;
END;
$$;
/

CALL test_proc1();


-- error: can't return non-NULL
CREATE PROCEDURE test_proc2()
LANGUAGE plisql
AS $$
BEGIN
    RETURN 5;
END;
$$;
/


CREATE TABLE test1 (a int);

CREATE PROCEDURE test_proc3(x int)
LANGUAGE plisql
AS $$
BEGIN
    INSERT INTO test1 VALUES (x);
END;
$$;
/

CALL test_proc3(55);

SELECT * FROM test1;


-- Check that plan revalidation doesn't prevent setting transaction properties
-- (bug #18059).  This test must include the first temp-object creation in
-- this script, or it won't exercise the bug scenario.  Hence we put it early.
CREATE PROCEDURE test_proc3a()
LANGUAGE plisql
AS $$
BEGIN
   COMMIT;
   SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;
   RAISE NOTICE 'done';
END;
$$;
/

CALL test_proc3a();
CREATE TEMP TABLE tt1(f1 int);
CALL test_proc3a();


-- nested CALL
TRUNCATE TABLE test1;

CREATE PROCEDURE test_proc4(y int)
LANGUAGE plisql
AS $$
BEGIN
     test_proc3(y);
     test_proc3($1);
END;
$$;
/

CALL test_proc4(66);

SELECT * FROM test1;

CALL test_proc4(66);

SELECT * FROM test1;


-- output arguments

CREATE PROCEDURE test_proc5(INOUT a text)
LANGUAGE plisql
AS $$
BEGIN
    a := a || '+' || a;
END;
$$;
/

CALL test_proc5('abc');


CREATE PROCEDURE test_proc6(a int, INOUT b int, INOUT c int)
LANGUAGE plisql
AS $$
BEGIN
    b := b * a;
    c := c * a;
END;
$$;
/

CALL test_proc6(2, 3, 4);


DO
LANGUAGE plisql
$$
DECLARE
    x int := 3;
    y int := 4;
BEGIN
     test_proc6(2, x, y);
    RAISE INFO 'x = %, y = %', x, y;
     test_proc6(2, c => y, b => x);
    RAISE INFO 'x = %, y = %', x, y;
END;
$$;


DO
LANGUAGE plisql
$$
DECLARE
    x int := 3;
    y int := 4;
BEGIN
     test_proc6(2, x + 1, y);  -- error
    RAISE INFO 'x = %, y = %', x, y;
END;
$$;


DO
LANGUAGE plisql
$$
DECLARE
    x constant int := 3;
    y int := 4;
BEGIN
     test_proc6(2, x, y);  -- error because x is constant
END;
$$;


DO
LANGUAGE plpgsql
$$
DECLARE
    x int := 3;
    y int := 4;
BEGIN
    FOR i IN 1..5 LOOP
        CALL test_proc6(i, x, y);
        RAISE INFO 'x = %, y = %', x, y;
    END LOOP;
END;
$$;


-- recursive with output arguments

CREATE PROCEDURE test_proc7(x int, INOUT a int, INOUT b numeric)
LANGUAGE plisql
AS $$
BEGIN
IF x > 1 THEN
    a := x / 10;
    b := x / 2;
     test_proc7(b::int, a, b);
END IF;
END;
$$;
/

CALL test_proc7(100, -1, -1);

-- inner COMMIT with output arguments

CREATE PROCEDURE test_proc7c(x int, INOUT a int, INOUT b numeric)
LANGUAGE plisql
AS $$
BEGIN
  a := x / 10;
  b := x / 2;
  COMMIT;
END;
$$;
/

CREATE PROCEDURE test_proc7cc(_x int)
LANGUAGE plisql
AS $$
DECLARE _a int; _b numeric;
BEGIN
   test_proc7c(_x, _a, _b);
  RAISE NOTICE '_x: %,_a: %, _b: %', _x, _a, _b;
END
$$;
/

CALL test_proc7cc(10);


-- named parameters and defaults

CREATE PROCEDURE test_proc8a(INOUT a int, INOUT b int)
LANGUAGE plisql
AS $$
BEGIN
  RAISE NOTICE 'a: %, b: %', a, b;
  a := a * 10;
  b := b + 10;
END;
$$;
/

CALL test_proc8a(10, 20);
CALL test_proc8a(b => 20, a => 10);

DO $$
DECLARE _a int; _b int;
BEGIN
  _a := 10; _b := 30;
   test_proc8a(_a, _b);
  RAISE NOTICE '_a: %, _b: %', _a, _b;
   test_proc8a(b => _b, a => _a);
  RAISE NOTICE '_a: %, _b: %', _a, _b;
END
$$;


CREATE PROCEDURE test_proc8b(INOUT a int, INOUT b int, INOUT c int)
LANGUAGE plisql
AS $$
BEGIN
  RAISE NOTICE 'a: %, b: %, c: %', a, b, c;
  a := a * 10;
  b := b + 10;
  c := c * -10;
END;
$$;
/

DO $$
DECLARE _a int; _b int; _c int;
BEGIN
  _a := 10; _b := 30; _c := 50;
   test_proc8b(_a, _b, _c);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
   test_proc8b(_a, c => _c, b => _b);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
END
$$;


CREATE PROCEDURE test_proc8c(INOUT a int, INOUT b int, INOUT c int)
LANGUAGE plisql
AS $$
BEGIN
  RAISE NOTICE 'a: %, b: %, c: %', a, b, c;
  a := a * 10;
  b := b + 10;
  c := c * -10;
END;
$$;
/

DO $$
DECLARE _a int; _b int; _c int;
BEGIN
  _a := 10; _b := 30; _c := 50;
   test_proc8c(_a, _b, _c);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
  _a := 10; _b := 30; _c := 50;
   test_proc8c(_a, c => _c, b => _b);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
  _a := 10; _b := 30; _c := 50;
   test_proc8c(c => _c, b => _b, a => _a);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
END
$$;

DO $$
DECLARE _a int; _b int; _c int;
BEGIN
  _a := 10; _b := 30; _c := 50;
   test_proc8c(_a, _b);  -- fail, no output argument for c
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
END
$$;

DO $$
DECLARE _a int; _b int; _c int;
BEGIN
  _a := 10; _b := 30; _c := 50;
   test_proc8c(_a, b => _b);  -- fail, no output argument for c
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
END
$$;


-- OUT parameters

CREATE PROCEDURE test_proc9(IN a int, OUT b int)
LANGUAGE plisql
AS $$
BEGIN
  RAISE NOTICE 'a: %, b: %', a, b;
  b := a * 2;
END;
$$;
/

DO $$
DECLARE _a int; _b int;
BEGIN
  _a := 10; _b := 30;
   test_proc9(_a, _b);
  RAISE NOTICE '_a: %, _b: %', _a, _b;
END
$$;

CREATE PROCEDURE test_proc10(IN a int, OUT b int, IN c int DEFAULT 11)
LANGUAGE plisql
AS $$
BEGIN
  RAISE NOTICE 'a: %, b: %, c: %', a, b, c;
  b := a - c;
END;
$$;
/

DO $$
DECLARE _a int; _b int; _c int;
BEGIN
  _a := 10; _b := 30; _c := 7;
   test_proc10(_a, _b, _c);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;

  _a := 10; _b := 30; _c := 7;
   test_proc10(_a, _b, c => _c);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;

  _a := 10; _b := 30; _c := 7;
   test_proc10(a => _a, b => _b, c => _c);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;

  _a := 10; _b := 30; _c := 7;
   test_proc10(_a, c => _c, b => _b);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;

  _a := 10; _b := 30; _c := 7;
   test_proc10(_a, _b);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;

  _a := 10; _b := 30; _c := 7;
   test_proc10(_a, b => _b);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;

  _a := 10; _b := 30; _c := 7;
   test_proc10(b => _b, a => _a);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
END
$$;

-- OUT + VARIADIC

CREATE PROCEDURE test_proc11(a OUT int, VARIADIC b int[])
LANGUAGE plisql
AS $$
BEGIN
  RAISE NOTICE 'a: %, b: %', a, b;
  a := b[1] + b[2];
END;
$$;
/

DO $$
DECLARE _a int; _b int; _c int;
BEGIN
  _a := 10; _b := 30; _c := 7;
   test_proc11(_a, _b, _c);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
END
$$;

-- polymorphic OUT arguments

CREATE PROCEDURE test_proc12(a anyelement, OUT b anyelement, OUT c anyarray)
LANGUAGE plpgsql
AS $$
BEGIN
  RAISE NOTICE 'a: %', a;
  b := a;
  c := array[a];
END;
$$;
/

DO $$
DECLARE _a int; _b int; _c int[];
BEGIN
  _a := 10;
  CALL test_proc12(_a, _b, _c);
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
END
$$;

DO $$
DECLARE _a int; _b int; _c text[];
BEGIN
  _a := 10;
  CALL test_proc12(_a, _b, _c);  -- error
  RAISE NOTICE '_a: %, _b: %, _c: %', _a, _b, _c;
END
$$;


-- transition variable assignment

TRUNCATE test1;

CREATE FUNCTION triggerfunc1() RETURNS trigger
LANGUAGE plisql
AS $$
DECLARE
    z int := 0;
BEGIN
     test_proc6(2, NEW.a, NEW.a);
    RETURN NEW;
END;
$$;
/

CREATE TRIGGER t1 BEFORE INSERT ON test1 EXECUTE PROCEDURE triggerfunc1();

INSERT INTO test1 VALUES (1), (2), (3);

UPDATE test1 SET a = 22 WHERE a = 2;

SELECT * FROM test1 ORDER BY a;


DROP PROCEDURE test_proc1;
DROP PROCEDURE test_proc3;
DROP PROCEDURE test_proc4;

DROP TABLE test1;


-- more checks for named-parameter handling

CREATE PROCEDURE p1(v_cnt int, v_Text inout text)
AS $$
BEGIN
  v_Text := 'v_cnt = ' || v_cnt;
END
$$ LANGUAGE plisql;
/

DO $$
DECLARE
  v_Text text;
  v_cnt  integer := 42;
BEGIN
   p1(v_cnt := v_cnt);  -- error, must supply something for v_Text
  RAISE NOTICE '%', v_Text;
END;
$$;

DO $$
DECLARE
  v_Text text;
  v_cnt  integer := 42;
BEGIN
   p1(v_cnt := v_cnt, v_Text := v_Text);
  RAISE NOTICE '%', v_Text;
END;
$$;

DO $$
DECLARE
  v_Text text;
BEGIN
   p1(10, v_Text := v_Text);
  RAISE NOTICE '%', v_Text;
END;
$$;

DO $$
DECLARE
  v_Text text;
  v_cnt  integer;
BEGIN
   p1(v_Text := v_Text, v_cnt := v_cnt);
  RAISE NOTICE '%', v_Text;
END;
$$;
