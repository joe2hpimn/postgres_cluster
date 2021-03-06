SET synchronous_commit = on;

-- This file doesn't share common setup with the native tests,
-- since it's specific to how the text protocol handles encodings.

CREATE TABLE enctest(blah text);

SELECT 'init' FROM pg_create_logical_replication_slot('regression_slot', 'pglogical_output');


SET client_encoding = 'UTF-8';
INSERT INTO enctest(blah)
VALUES
('áàä'),('ﬂ'), ('½⅓'), ('カンジ');
RESET client_encoding;


SET client_encoding = 'LATIN-1';

-- Will ERROR, explicit encoding request doesn't match client_encoding
SELECT data
FROM pg_logical_slot_peek_changes('regression_slot',
	NULL, NULL,
	'expected_encoding', 'UTF8',
	'min_proto_version', '1',
	'max_proto_version', '1',
	'startup_params_format', '1',
	'proto_format', 'json',
	'no_txinfo', 't');

-- Will succeed since we don't request any encoding
-- then ERROR because it can't turn the kanjii into latin-1
SELECT data
FROM pg_logical_slot_peek_changes('regression_slot',
	NULL, NULL,
	'min_proto_version', '1',
	'max_proto_version', '1',
	'startup_params_format', '1',
	'proto_format', 'json',
	'no_txinfo', 't');

-- Will succeed since it matches the current encoding
-- then ERROR because it can't turn the kanjii into latin-1
SELECT data
FROM pg_logical_slot_peek_changes('regression_slot',
	NULL, NULL,
	'expected_encoding', 'LATIN-1',
	'min_proto_version', '1',
	'max_proto_version', '1',
	'startup_params_format', '1',
	'proto_format', 'json',
	'no_txinfo', 't');

RESET client_encoding;

SELECT 'drop' FROM pg_drop_replication_slot('regression_slot');

DROP TABLE enctest;
