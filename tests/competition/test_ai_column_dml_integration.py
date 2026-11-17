"""Exercise native transactional AI COLUMN DML; no SQL interception or worker.
Run on a private seekdb socket via SEEKDB_TEST_SOCKET. No endpoint is installed,
so task creation must not require HTTP and workers cannot drain these fixtures.
"""
import json
import os
import unittest
import uuid

import pymysql


class AIColumnDMLIntegration(unittest.TestCase):
    def setUp(self):
        self.db = pymysql.connect(unix_socket=os.environ['SEEKDB_TEST_SOCKET'],
                                 user='root', database='test', autocommit=True,
                                 charset='utf8mb4', read_timeout=30, write_timeout=30)
        self.addCleanup(self.db.close)
        self.name = 'aicdml_' + uuid.uuid4().hex[:16]
        self.model = self.name + '_model'
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                 (self.model, json.dumps({'type': 'completion', 'model_name': 'no-http-fixture'})))
        self.addCleanup(self.sql, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (self.model,))
        self.addCleanup(self.sql, f'DROP TABLE IF EXISTS `{self.name}`')
        self.sql(f'''CREATE TABLE `{self.name}` (id BIGINT PRIMARY KEY,
            body VARCHAR(200), title VARCHAR(100),
            tags JSON AI COLUMN (AI_COMPLETE(%s, CONCAT('Classify: ',body)))
              WITH (sync_mode='async',max_retries=2))''', (self.model,))
        self.table_id = self.sql('''SELECT t.table_id FROM oceanbase.__all_table t
            JOIN oceanbase.__all_database d ON d.database_id=t.database_id
            WHERE d.database_name='test' AND t.table_name=%s''', (self.name,))[0][0]

    def sql(self, query, args=None):
        with self.db.cursor() as cur:
            cur.execute(query, args)
            return cur.fetchall()

    def tasks(self):
        return self.sql('''SELECT rowkey_predicate,input,state,retry_count,
            request_id,group_id,definition_version FROM oceanbase.__all_ai_column_task
            WHERE table_id=%s ORDER BY rowkey_predicate''', (self.table_id,))

    def test_first_insert_initializes_helpers_and_keeps_session_usable(self):
        # Regression: begin_nested_session alone is not enough to classify the
        # nested task DELETE as nested DML; its close must not end the outer stmt.
        self.sql('START TRANSACTION')
        try:
            self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', ('first',))
            # Helpers are deliberately hidden from ordinary SQL name resolution;
            # inspect the durable task/spec and visible result, not a privileged
            # SELECT of hidden helper values.
            self.assertEqual(self.sql(f'SELECT tags IS NULL FROM `{self.name}` WHERE id=1'), ((1,),))
            task = self.tasks()[0]
            self.assertEqual(task[2], 0)
            self.assertTrue(task[4])
            self.assertEqual(task[4], task[5])
            spec = self.sql('''SELECT result_spec,column_id FROM oceanbase.__all_ai_column_task
                WHERE table_id=%s''', (self.table_id,))[0]
            self.assertEqual(json.loads(spec[0])['column_id'],spec[1])
            self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(2,%s)', ('next statement',))
            self.assertEqual(len(self.tasks()), 2)
        finally:
            self.sql('ROLLBACK')
        self.assertEqual(self.tasks(), ())
        self.assertEqual(self.sql(f'SELECT COUNT(*) FROM `{self.name}`'), ((0,),))
        self.assertEqual(self.sql('SELECT 1'), ((1,),))
        # A leaked private capability would let this ordinary user statement
        # bypass the protected helper write check after the nested session ends.
        with self.assertRaises(pymysql.MySQLError):
            self.sql(f'UPDATE `{self.name}` SET `__ai_tags_state`=1 WHERE id=1')

    def test_failed_batch_rolls_back_only_current_statement(self):
        self.sql('START TRANSACTION')
        try:
            self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', ('preserved',))
            initial = self.tasks()
            with self.assertRaises(pymysql.IntegrityError) as failed:
                # The first candidate can enqueue before the duplicate candidate
                # fails. Neither its row nor task may escape statement rollback.
                self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(2,%s),(1,%s)',
                         ('must rollback', 'duplicate'))
            self.assertEqual(failed.exception.args[0], 1062)
            self.assertEqual(self.tasks(), initial)
            self.assertEqual(self.sql(f'SELECT id,body FROM `{self.name}` ORDER BY id'),
                             ((1, 'preserved'),))
            self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(3,%s)', ('still usable',))
            self.assertEqual(len(self.tasks()), 2)
        finally:
            self.sql('ROLLBACK')
        self.assertEqual(self.tasks(), ())
        self.assertEqual(self.sql(f'SELECT COUNT(*) FROM `{self.name}`'), ((0,),))

    def test_insert_queue_same_transaction_rollback(self):
        self.sql('START TRANSACTION')
        try:
            self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', ('first',))
            self.assertEqual(self.sql(f'SELECT tags IS NULL FROM `{self.name}`'), ((1,),))
            tasks = self.tasks()
            self.assertEqual(len(tasks), 1, 'native INSERT must enqueue in the user transaction')
            self.assertEqual(tasks[0][:4], ('`id` = 1', 'Classify: first', 0, 0))
            self.assertTrue(tasks[0][4])
            self.assertEqual(tasks[0][4], tasks[0][5])
            self.assertEqual(tasks[0][6], 1)
        finally:
            self.sql('ROLLBACK')
        self.assertEqual(self.tasks(), ())
        self.assertEqual(self.sql(f'SELECT COUNT(*) FROM `{self.name}`'), ((0,),))


    def test_unrelated_update_preserves_generation_dependency_refreshes(self):
        self.sql('START TRANSACTION')
        try:
            self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', ('first',))
            initial = self.tasks()
            self.sql(f'UPDATE `{self.name}` SET title=%s WHERE id=1', ('unrelated',))
            self.assertEqual(self.tasks(), initial)
            self.sql(f'UPDATE `{self.name}` SET body=%s WHERE id=1', ('new version',))
            tasks = self.tasks()
            self.assertEqual(len(tasks), 1)
            self.assertEqual(tasks[0][1], 'Classify: new version')
            self.assertNotEqual(tasks[0][4], initial[0][4])
        finally:
            self.sql('ROLLBACK')

    def test_primary_key_update_and_batch_delete(self):
        self.sql('START TRANSACTION')
        try:
            self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(10,%s),(11,%s)', ('a', 'b'))
            initial = self.tasks()
            self.assertEqual(len(initial), 2)
            self.sql(f'UPDATE `{self.name}` SET id=12 WHERE id=10')
            tasks = self.tasks()
            self.assertEqual({row[0] for row in tasks}, {'`id` = 11', '`id` = 12'})
            self.assertNotEqual(next(row[4] for row in tasks if row[0] == '`id` = 12'), initial[0][4])
            self.sql(f'DELETE FROM `{self.name}` WHERE id IN (11,12)')
            self.assertEqual(self.tasks(), ())
        finally:
            self.sql('ROLLBACK')

    def test_null_input_and_upsert_actual_branch(self):
        self.sql('START TRANSACTION')
        try:
            self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,NULL),(2,%s)', ('second',))
            self.assertEqual([t[1] for t in self.tasks()], [None, 'Classify: second'])
            old_request = self.tasks()[0][4]
            self.sql(f'''INSERT INTO `{self.name}`(id,body) VALUES(1,%s)
                ON DUPLICATE KEY UPDATE body=VALUES(body)''', ('upserted',))
            tasks = self.tasks()
            self.assertEqual(len(tasks), 2)
            self.assertEqual(tasks[0][1], 'Classify: upserted')
            self.assertNotEqual(tasks[0][4], old_request)
            self.sql(f'''INSERT INTO `{self.name}`(id,title) VALUES(1,%s)
                ON DUPLICATE KEY UPDATE title=VALUES(title)''', ('unrelated upsert',))
            self.assertEqual(self.tasks(), tasks)
        finally:
            self.sql('ROLLBACK')

    def test_uncommitted_queue_is_invisible_to_worker_connection(self):
        observer = pymysql.connect(unix_socket=os.environ['SEEKDB_TEST_SOCKET'],
                                   user='root', database='test', autocommit=True)
        self.addCleanup(observer.close)
        self.sql('START TRANSACTION')
        try:
            self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', ('uncommitted',))
            self.assertEqual(len(self.tasks()), 1)
            with observer.cursor() as cur:
                cur.execute('SELECT COUNT(*) FROM oceanbase.__all_ai_column_task WHERE table_id=%s', (self.table_id,))
                self.assertEqual(cur.fetchall(), ((0,),))
        finally:
            self.sql('ROLLBACK')
        self.assertEqual(self.tasks(), ())

    def test_multiple_columns_share_request_and_group(self):
        self.sql(f'''ALTER TABLE `{self.name}` ADD summary JSON AI COLUMN
            (AI_COMPLETE(%s,body)) WITH(sync_mode='async',max_retries=2)''', (self.model,))
        self.sql('START TRANSACTION')
        try:
            self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', ('shared',))
            tasks = self.tasks()
            self.assertEqual(len(tasks), 2)
            self.assertEqual(len({t[4] for t in tasks}), 1)
            self.assertTrue(all(t[4] == t[5] for t in tasks))
            rows = self.sql('''SELECT column_id,group_membership,result_spec FROM
                oceanbase.__all_ai_column_task WHERE table_id=%s''', (self.table_id,))
            member_ids = {r[0] for r in rows}
            for column_id, membership, spec in rows:
                self.assertEqual(set(json.loads(membership)), member_ids)
                spec = json.loads(spec)
                self.assertEqual(spec['version'], 2)
                self.assertEqual(spec['input_kind'], 'text')
                self.assertEqual(spec['column_id'], column_id)
                self.assertEqual(spec['model_name'], self.model)
        finally:
            self.sql('ROLLBACK')

    def test_sql_user_cannot_write_result_or_helpers(self):
        self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', ('first',))
        for assignment in ['tags=NULL', '`__ai_tags_state`=1',
                           "`__ai_tags_request`='forged'", "`__ai_tags_result_spec`='{}'"]:
            with self.subTest(assignment=assignment):
                with self.assertRaises(pymysql.MySQLError):
                    self.sql(f'UPDATE `{self.name}` SET {assignment} WHERE id=1')
        with self.assertRaises(pymysql.MySQLError):
            self.sql(f'INSERT INTO `{self.name}`(id,body,tags) VALUES(2,%s,NULL)', ('forged',))


if __name__ == '__main__':
    unittest.main()
