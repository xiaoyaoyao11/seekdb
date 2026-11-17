"""Real DDL/worker regressions. Requires SEEKDB_TEST_SOCKET; owns no seekdb server.

Run in the server's network namespace. HTTP is the sole recording fixture.
"""
import json
import os
import time
import unittest
import uuid

import pymysql

from test_ai_column_worker_integration import RecordingModel


class AIColumnDDLEdges(unittest.TestCase):
    def setUp(self):
        self.db = pymysql.connect(unix_socket=os.environ['SEEKDB_TEST_SOCKET'],
                                 user='root', database='test', autocommit=True,
                                 charset='utf8mb4', read_timeout=30, write_timeout=30)
        self.addCleanup(self.db.close)
        self.sql('SET SESSION ai_column_strict_read=OFF')
        self.sql("SET SESSION sql_mode=''")
        self.name = 'aicddl_' + uuid.uuid4().hex[:12]
        # Deliberately includes both SQL escape-sensitive characters.
        self.model = self.name + "\\'模型"
        self.endpoint = self.name + '_ep'
        self.http = RecordingModel()
        self.addCleanup(self.http.close)
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                 (self.model, json.dumps(dict(type='completion', model_name='fixture'))))
        self.addCleanup(self.sql, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (self.model,))
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                 (self.endpoint, json.dumps(dict(ai_model_name=self.model,
                  url=f'http://127.0.0.1:{self.http.server.server_port}/v1/chat/completions',
                  access_key='local-fixture', provider='openai', request_model_name=self.name))))
        self.addCleanup(self.sql, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', (self.endpoint,))
        self.addCleanup(self.sql, f'DROP TABLE IF EXISTS `{self.name}_copy`')
        self.addCleanup(self.sql, f'DROP TABLE IF EXISTS `{self.name}`')
        # DBMS_AI_SERVICE cleanup is outside this DDL regression: restore the
        # default mode before its internal model-name SQL handles odd names.
        self.addCleanup(self.sql, "SET SESSION sql_mode=''")

    def sql(self, query, args=None):
        with self.db.cursor() as cur:
            cur.execute(query, args)
            return cur.fetchall()

    def create(self):
        self.sql(f'''CREATE TABLE `{self.name}` (id BIGINT PRIMARY KEY, body VARCHAR(200),
          answer JSON AI COLUMN(AI_COMPLETE(%s,body)) WITH(max_retries=0))''', (self.model,))

    def snapshot(self):
        table_id = self.sql('SELECT table_id FROM oceanbase.__all_table WHERE table_name=%s',
                            (self.name,))[0][0]
        return (self.sql(f'SHOW CREATE TABLE `{self.name}`'),
                self.sql('SELECT * FROM oceanbase.__all_ai_column WHERE table_id=%s', (table_id,)),
                self.sql('SELECT * FROM oceanbase.__all_column WHERE table_id=%s ORDER BY column_id',
                         (table_id,)))

    def assert_rejected_unchanged(self, clause, args=None):
        before = self.snapshot()
        with self.assertRaises(pymysql.MySQLError) as raised:
            self.sql(f'ALTER TABLE `{self.name}` {clause}', args)
        self.assertEqual(raised.exception.args[0], 1235)
        self.assertEqual(self.snapshot(), before, 'rejected DDL must not mutate definitions or helpers')

    def test_modify_change_cannot_silently_add_ai_attributes(self):
        self.create()
        for clause in ('MODIFY COLUMN body VARCHAR(200)',
                       'CHANGE COLUMN body renamed VARCHAR(200)'):
            with self.subTest(clause=clause):
                self.assert_rejected_unchanged(clause +
                    ' AI COLUMN(AI_COMPLETE(%s,\'replacement\')) WITH(max_retries=2)', (self.model,))

    def test_modify_change_cannot_silently_replace_ai_definition(self):
        self.create()
        for clause in ('MODIFY COLUMN answer JSON', 'CHANGE COLUMN answer renamed JSON'):
            with self.subTest(clause=clause):
                self.assert_rejected_unchanged(clause +
                    ' AI COLUMN(AI_COMPLETE(%s,CONCAT(\'replacement\',body))) WITH(max_retries=2)',
                    (self.model,))

    def test_ordinary_modify_change_of_ai_result_preserves_helper_invariants(self):
        self.create()
        for clause in ('MODIFY COLUMN answer JSON', 'MODIFY COLUMN answer JSON NOT NULL',
                       'MODIFY COLUMN answer BIGINT', 'MODIFY COLUMN answer JSON INVISIBLE',
                       'CHANGE COLUMN answer renamed JSON'):
            with self.subTest(clause=clause):
                self.assert_rejected_unchanged(clause)

    def wait_result(self, table, row_id, expected_user):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            value = self.sql(f'SELECT answer FROM `{table}` WHERE id=%s', (row_id,))[0][0]
            if value is not None:
                self.assertEqual(json.loads(value), {'input': expected_user})
                with self.http.lock:
                    events = list(self.http.events)
                matching = [e for e in events if e['user'] == expected_user]
                self.assertTrue(matching, f'exact UTF8 user bytes absent from provider: {events!r}')
                self.assertTrue(all(e['body']['model'] == self.name for e in matching),
                                'SHOW CREATE must retain the original model binding')
                self.assertTrue(all('valid JSON value' in e['system'] for e in matching))
                return
            time.sleep(.05)
        self.fail(f'native worker did not publish exact input {expected_user!r}; events={self.http.events!r}')

    def literal_roundtrip(self, defining_mode):
        self.sql('SET SESSION sql_mode=%s', (defining_mode,))
        # This is a literal JSON envelope, NOT an AI_PROMPT to render recursively.
        literal = json.dumps({'template': "keep {0} \\n \\ ' 中😀", 'args': ["x\\'y"]},
                             ensure_ascii=False) + " apostrophe:' backslash:\\ "
        self.sql(f'''CREATE TABLE `{self.name}` (id BIGINT PRIMARY KEY, body VARCHAR(200),
          answer JSON AI COLUMN(AI_COMPLETE(%s,CONCAT(%s,body))) WITH(max_retries=0))''',
                 (self.model, literal))
        # Execute persisted input under BOTH business SQL modes; the worker uses
        # its own internal session to evaluate the persisted typed config SQL.
        for row_id, mode in enumerate(('', 'NO_BACKSLASH_ESCAPES'), 1):
            self.sql('SET SESSION sql_mode=%s', (mode,))
            suffix = f'original-{row_id}'
            self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(%s,%s)', (row_id, suffix))
            self.wait_result(self.name, row_id, literal + suffix)
        # Print under the opposite mode, then recreate and execute in both modes.
        other_mode = '' if defining_mode else 'NO_BACKSLASH_ESCAPES'
        self.sql('SET SESSION sql_mode=%s', (other_mode,))
        create = self.sql(f'SHOW CREATE TABLE `{self.name}`')[0][1]
        copy = self.name + '_copy'
        self.sql(create.replace(f'CREATE TABLE `{self.name}`', f'CREATE TABLE `{copy}`', 1))
        for row_id, mode in enumerate(('', 'NO_BACKSLASH_ESCAPES'), 1):
            self.sql('SET SESSION sql_mode=%s', (mode,))
            suffix = f'copy-{row_id}'
            self.sql(f'INSERT INTO `{copy}`(id,body) VALUES(%s,%s)', (row_id, suffix))
            self.wait_result(copy, row_id, literal + suffix)

    def test_envelope_shaped_literal_is_not_rendered(self):
        literal = json.dumps({'template': "do not render {0} \\ ' 中😀", 'args': ['value']},
                             ensure_ascii=False)
        self.sql(f'''CREATE TABLE `{self.name}` (id BIGINT PRIMARY KEY,
          answer JSON AI COLUMN(AI_COMPLETE(%s,%s)) WITH(max_retries=0))''',
                 (self.model, literal))
        for row_id, mode in enumerate(('', 'NO_BACKSLASH_ESCAPES'), 1):
            self.sql('SET SESSION sql_mode=%s', (mode,))
            self.sql(f'INSERT INTO `{self.name}`(id) VALUES(%s)', (row_id,))
            self.wait_result(self.name, row_id, literal)

    def test_default_mode_literal_bytes_and_show_create_roundtrip(self):
        self.literal_roundtrip('')

    def test_no_backslash_mode_literal_bytes_and_show_create_roundtrip(self):
        self.literal_roundtrip('NO_BACKSLASH_ESCAPES')


if __name__ == '__main__':
    unittest.main()
