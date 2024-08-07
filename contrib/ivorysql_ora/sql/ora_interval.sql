--
-- INTERVAL YEAR TO MONTH
--

-- Valid Interval Literals
SELECT INTERVAL'1-1' YEAR TO MONTH;

SELECT INTERVAL '123-2' YEAR(3) TO MONTH;

SELECT INTERVAL'11' YEAR;

SELECT INTERVAL'356' YEAR(3);

SELECT INTERVAL'12' MONTH;

SELECT INTERVAL'1200' MONTH(3);


-- If the leading field is YEAR and the trailing field is MONTH, 
-- then the range of integer values for the month field is 0 to 11.
SELECT INTERVAL'1-11' YEAR TO MONTH;

SELECT INTERVAL'1-12' YEAR TO MONTH;	

CREATE TABLE TEST_YMINTERVAL(a interval year(4) to month);

INSERT INTO TEST_YMINTERVAL (a) VALUES ('0-1');

INSERT INTO TEST_YMINTERVAL (a) VALUES ('1-2');

INSERT INTO TEST_YMINTERVAL (a) VALUES ('12-3');

INSERT INTO TEST_YMINTERVAL (a) VALUES ('123-4');

INSERT INTO TEST_YMINTERVAL (a) VALUES ('1234-5');

-- Precision out of range
INSERT INTO TEST_YMINTERVAL (a) VALUES ('12345-6');


-- Comparison operator
SELECT * FROM TEST_YMINTERVAL WHERE TEST_YMINTERVAL.a = interval'12-3' year to month;

SELECT * FROM TEST_YMINTERVAL WHERE TEST_YMINTERVAL.a <> interval'12-3' year to month;

SELECT * FROM TEST_YMINTERVAL WHERE TEST_YMINTERVAL.a > interval'12-3' year to month;

SELECT * FROM TEST_YMINTERVAL WHERE TEST_YMINTERVAL.a >= interval'12-3' year to month;

SELECT * FROM TEST_YMINTERVAL WHERE TEST_YMINTERVAL.a < interval'12-3' year to month;

SELECT * FROM TEST_YMINTERVAL WHERE TEST_YMINTERVAL.a <= interval'12-3' year to month;


-- "+" operator
SELECT TEST_YMINTERVAL.a + interval'1-1' year to month FROM TEST_YMINTERVAL;

-- "-" operator
SELECT TEST_YMINTERVAL.a - interval'1-1' year to month FROM TEST_YMINTERVAL;


-- AGGREGATE
SELECT MAX(a) FROM TEST_YMINTERVAL;

SELECT MIN(a) FROM TEST_YMINTERVAL;


-- index 
CREATE INDEX test_yminterval_btree on TEST_YMINTERVAL(a);

CREATE INDEX test_yminterval_hash on TEST_YMINTERVAL USING hash (a);

CREATE INDEX test_yminterval_brin on TEST_YMINTERVAL USING brin (a);


--
-- INTERVAL DAY TO SECOND
--

-- Valid Interval Literals
SELECT INTERVAL '4 5:12:10.222' DAY TO SECOND(3);

SELECT INTERVAL '4 5:12' DAY TO MINUTE;

SELECT INTERVAL '400 5' DAY(3) TO HOUR;

SELECT INTERVAL '400' DAY(3);

SELECT INTERVAL '11:12:10.2222222' HOUR TO SECOND(7);

SELECT INTERVAL '11:20' HOUR TO MINUTE;

SELECT INTERVAL '10' HOUR;

SELECT INTERVAL '10:22' MINUTE TO SECOND;

SELECT INTERVAL '10' MINUTE;

SELECT INTERVAL '4' DAY;

SELECT INTERVAL '25' HOUR;

SELECT INTERVAL '40' MINUTE;

SELECT INTERVAL '120' HOUR(3);

SELECT INTERVAL '30.12345' SECOND(2,4);


-- The valid range of values for the trailing field are as follows:
-- HOUR: 0 to 23
-- MINUTE: 0 to 59
-- SECOND: 0 to 59.999999999
SELECT INTERVAL '400 24' DAY(3) TO HOUR;	--ERROR

SELECT INTERVAL '4 5:60' DAY TO MINUTE;		--ERROR

SELECT INTERVAL '4 5:12:60' DAY TO SECOND(3);	--ERROR	


CREATE TABLE TEST_DSINTERVAL(a interval day(4) to second(6));

INSERT INTO TEST_DSINTERVAL (a) VALUES ('0 0:0:0');

INSERT INTO TEST_DSINTERVAL (a) VALUES ('1 1:1:1.123');

INSERT INTO TEST_DSINTERVAL (a) VALUES ('12 1:1:1.123456');

INSERT INTO TEST_DSINTERVAL (a) VALUES ('123 1:1:1.123456789');

INSERT INTO TEST_DSINTERVAL (a) VALUES ('1234 1:1:1.123456789');

-- Precision out of range
INSERT INTO TEST_DSINTERVAL (a) VALUES ('12345 1:1:1.123456');


-- Comparison operator
SELECT * FROM TEST_DSINTERVAL WHERE TEST_DSINTERVAL.a = interval'12 1:1:1.123456' day to second;

SELECT * FROM TEST_DSINTERVAL WHERE TEST_DSINTERVAL.a <> interval'12 1:1:1.123456' day to second;

SELECT * FROM TEST_DSINTERVAL WHERE TEST_DSINTERVAL.a > interval'12 1:1:1.123456' day to second;

SELECT * FROM TEST_DSINTERVAL WHERE TEST_DSINTERVAL.a >= interval'12 1:1:1.123456' day to second;

SELECT * FROM TEST_DSINTERVAL WHERE TEST_DSINTERVAL.a < interval'12 1:1:1.123456' day to second;

SELECT * FROM TEST_DSINTERVAL WHERE TEST_DSINTERVAL.a <= interval'12 1:1:1.123456' day to second;


-- "+" operator
SELECT TEST_DSINTERVAL.a + interval'1 1:1:1' day to second FROM TEST_DSINTERVAL;

-- "-" operator
SELECT TEST_DSINTERVAL.a - interval'1 1:1:1' day to second FROM TEST_DSINTERVAL;

-- AGGREGATE
SELECT MAX(a) FROM TEST_DSINTERVAL;

SELECT MIN(a) FROM TEST_DSINTERVAL;


-- index
CREATE INDEX test_dsinterval_btree on TEST_DSINTERVAL(a);

CREATE INDEX test_dsinterval_hash on TEST_DSINTERVAL USING hash (a);

CREATE INDEX test_dsinterval_brin on TEST_DSINTERVAL USING brin (a);


-- drop table
DROP TABLE TEST_YMINTERVAL;

DROP TABLE TEST_DSINTERVAL;
