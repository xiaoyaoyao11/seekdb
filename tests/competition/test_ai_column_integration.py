"""Native AI COLUMN SQL regressions. Requires a private socket-only instance."""
import json
import os
import unittest
import uuid

import pymysql


class AIColumnIntegration(unittest.TestCase):
    def setUp(self):
        self.db = pymysql.connect(unix_socket=os.environ['SEEKDB_TEST_SOCKET'],
                                 user='root', database='test', autocommit=True,
                                 charset='utf8mb4', read_timeout=30, write_timeout=30)
        self.addCleanup(self.db.close)
        self.name = 'aicol_' + uuid.uuid4().hex[:16]
        self.model = self.name + '_model'
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                 (self.model, json.dumps({'type': 'completion', 'model_name': 'fixture-completion'})))
        self.addCleanup(self.sql, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (self.model,))
        self.addCleanup(self.sql, f'DROP TABLE IF EXISTS `{self.name}`')

    def sql(self, query, args=None):
        with self.db.cursor() as cur:
            cur.execute(query, args)
            return cur.fetchall()

    def test_strict_read_defaults_off_and_session_scoped(self):
        self.assertEqual(self.sql('SELECT @@session.ai_column_strict_read'), ((0,),))
        self.sql('SET SESSION ai_column_strict_read=ON')
        self.assertEqual(self.sql('SELECT @@session.ai_column_strict_read'), ((1,),))
        with pymysql.connect(unix_socket=os.environ['SEEKDB_TEST_SOCKET'],user='root',
                             database='test',autocommit=True) as other:
            with other.cursor() as cur:
                cur.execute('SELECT @@session.ai_column_strict_read')
                self.assertEqual(cur.fetchall(), ((0,),))
        self.sql('SET SESSION ai_column_strict_read=OFF')
        self.assertEqual(self.sql('SELECT @@session.ai_column_strict_read'), ((0,),))

    def test_create_json_ai_column_metadata(self):
        self.sql(f'''CREATE TABLE `{self.name}` (
            id BIGINT PRIMARY KEY,
            body VARCHAR(200),
            tags JSON AI COLUMN (AI_COMPLETE(%s, CONCAT('Classify: ',body)))
                WITH (sync_mode='async',max_retries=2)
        )''', (self.model,))
        rows = self.sql('''SELECT a.column_name,a.max_retries,a.normalized_expr
            FROM oceanbase.__all_ai_column a
            JOIN oceanbase.__all_table t ON t.table_id=a.table_id
            JOIN oceanbase.__all_database d ON d.database_id=t.database_id
            WHERE d.database_name='test' AND t.table_name=%s''', (self.name,))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][:2], ('tags', 2))
        normalized = rows[0][2]
        if isinstance(normalized, bytes):
            normalized = normalized.decode('utf8')
        self.assertIn('Return exactly one valid JSON value', normalized)
        hidden = self.sql('''SELECT COUNT(*) FROM oceanbase.__all_column c
            JOIN oceanbase.__all_ai_column a ON a.table_id=c.table_id
              AND c.column_id IN (a.state_column_id,a.request_column_id,a.result_spec_column_id)
            JOIN oceanbase.__all_table t ON t.table_id=a.table_id
            WHERE t.table_name=%s AND c.is_hidden=1''', (self.name,))
        self.assertEqual(hidden, ((3,),))

    def test_alter_add_show_create_and_drop_cleanup(self):
        self.sql(f'CREATE TABLE `{self.name}` (id BIGINT PRIMARY KEY, body VARCHAR(200))')
        self.sql(f'''ALTER TABLE `{self.name}` ADD COLUMN answer BIGINT
            AI COLUMN(AI_COMPLETE(%s,body)) WITH(max_retries=1)''', (self.model,))
        create = self.sql(f'SHOW CREATE TABLE `{self.name}`')[0][1]
        self.assertIn('AI COLUMN', create)
        self.assertIn('AI_COMPLETE', create)
        self.assertNotIn('__ai_', create)
        table_id = self.sql('SELECT table_id FROM oceanbase.__all_table WHERE table_name=%s',
                            (self.name,))[0][0]
        self.assertEqual(self.sql('SELECT COUNT(*) FROM oceanbase.__all_ai_column WHERE table_id=%s',
                                  (table_id,)), ((1,),))
        self.sql(f'ALTER TABLE `{self.name}` DROP COLUMN answer')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM oceanbase.__all_ai_column WHERE table_id=%s',
                                  (table_id,)), ((0,),))
        self.assertEqual(self.sql('''SELECT COUNT(*) FROM oceanbase.__all_column
            WHERE table_id=%s AND column_name LIKE '__ai_%%' ''', (table_id,)), ((0,),))
        self.assertEqual(self.sql('''SELECT is_deleted FROM oceanbase.__all_ai_column_history
            WHERE table_id=%s ORDER BY schema_version DESC LIMIT 1''', (table_id,)), ((1,),))
        self.assertEqual(self.sql('SELECT COUNT(*) FROM oceanbase.__all_ai_column_task WHERE table_id=%s',
                                  (table_id,)), ((0,),))

    def test_task_catalog_runtime_contract(self):
        rows = self.sql('SHOW FULL COLUMNS FROM oceanbase.__all_ai_column_task')
        columns = {row[0]: row for row in rows}
        required = {'table_id', 'column_id', 'rowkey', 'request_id', 'rowkey_predicate',
                    'definition_version', 'group_id', 'group_membership', 'input',
                    'result_spec', 'state', 'retry_count', 'eligible_ts', 'lease_token',
                    'lease_expiry', 'staged_result', 'error_code', 'error'}
        self.assertTrue(required.issubset(columns))
        for name in ('table_id', 'column_id', 'rowkey', 'request_id'):
            self.assertEqual(columns[name][4], 'PRI')
            self.assertEqual(columns[name][3], 'NO')
        self.assertIn('varbinary', columns['rowkey'][1].lower())
        self.assertEqual(columns['eligible_ts'][1].lower(), 'datetime(6)')
        for name in ('lease_token', 'staged_result', 'error', 'input'):
            self.assertEqual(columns[name][3], 'YES')

    def test_ai_dependencies_rejected(self):
        with self.assertRaises(pymysql.MySQLError) as error:
            self.sql(f'''CREATE TABLE `{self.name}` (body VARCHAR(20),
                first_ai VARCHAR(200) AI COLUMN(AI_COMPLETE(%s,body)),
                second_ai VARCHAR(200) AI COLUMN(AI_COMPLETE(%s,first_ai)))''',
                (self.model,self.model))
        self.assertEqual(error.exception.args[0], 1235)

    def test_embedding_dimension_mismatch_rejected(self):
        with self.assertRaises(pymysql.MySQLError) as error:
            self.sql(f'''CREATE TABLE `{self.name}` (body VARCHAR(20),
                embedding VECTOR(4) AI COLUMN(AI_EMBED(%s,body,3)))''', (self.model,))
        self.assertEqual(error.exception.args[0], 7600)


if __name__ == '__main__':
    unittest.main()
