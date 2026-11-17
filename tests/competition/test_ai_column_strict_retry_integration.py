"""Real SQL checks for column-wide strict reads and predicate-selected retry.

A transaction keeps new tasks invisible to the worker, so PENDING assertions do
not depend on sleeps, provider latency or a running HTTP service.
"""
import json
import os
import unittest
import uuid

import pymysql


class AIColumnStrictRetry(unittest.TestCase):
    def setUp(self):
        self.db=pymysql.connect(unix_socket=os.environ['SEEKDB_TEST_SOCKET'],user='root',
            database='test',autocommit=True,charset='utf8mb4',read_timeout=30,write_timeout=30)
        self.addCleanup(self.db.close)
        self.table='strict_'+uuid.uuid4().hex[:16]
        self.model=self.table+'_model'
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
            (self.model,json.dumps({'type':'completion','model_name':'strict-test-model'})))
        self.addCleanup(self.sql,'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)',(self.model,))
        self.addCleanup(self.sql,'DROP TABLE IF EXISTS `'+self.table+'`')
        self.sql(f'''CREATE TABLE `{self.table}` (
            id BIGINT PRIMARY KEY, category VARCHAR(30), status VARCHAR(20), body VARCHAR(100),
            result VARCHAR(200) AI COLUMN (AI_COMPLETE(%s,body)) WITH (max_retries=0),
            label VARCHAR(200) AI COLUMN (AI_COMPLETE(%s,CONCAT('Label: ',body))) WITH (max_retries=0)
        )''',(self.model,self.model))
        self.sql('START TRANSACTION')
        self.addCleanup(self.db.rollback)
        self.sql(f"INSERT INTO `{self.table}`(id,category,status,body) VALUES (1,'news','active','one'),(2,'sports','inactive','two')")

    def sql(self, query, args=None):
        with self.db.cursor() as cur:
            cur.execute(query,args)
            return cur.fetchall()

    def strict_error(self, query, args=None):
        with self.assertRaises(pymysql.err.DatabaseError) as exc:
            self.sql(query,args)
        self.assertEqual(exc.exception.args[0],11120)
        self.assertRegex(exc.exception.args[1],
            r'^AI column has unfinished tasks for table_id=\d+, column_id=\d+$')

    def test_default_null_and_normal_columns_remain_readable(self):
        self.assertEqual(self.sql('SELECT @@session.ai_column_strict_read'),((0,),))
        self.assertEqual(self.sql(f'SELECT id,result,label FROM `{self.table}` ORDER BY id'),
                         ((1,None,None),(2,None,None)))
        self.sql('SET SESSION ai_column_strict_read=ON')
        self.assertEqual(self.sql(f'SELECT id,category FROM `{self.table}` ORDER BY id'),
                         ((1,'news'),(2,'sports')))
        self.assertEqual(self.sql(f'SELECT COUNT(*) FROM `{self.table}`'),((2,),))

    def test_reference_scope_is_column_wide_and_includes_subqueries(self):
        self.sql('SET SESSION ai_column_strict_read=ON')
        for query in (f'SELECT result FROM `{self.table}` WHERE id=999',
                      f'SELECT id FROM `{self.table}` WHERE result IS NULL',
                      f'SELECT id FROM `{self.table}` ORDER BY label LIMIT 0',
                      f'SELECT id FROM `{self.table}` WHERE id IN (SELECT id FROM `{self.table}` WHERE label IS NULL)',
                      f'SELECT id FROM `{self.table}` d WHERE EXISTS (SELECT 1 WHERE d.result IS NULL)',
                      f'SELECT id FROM `{self.table}` WHERE EXISTS (SELECT result FROM `{self.table}` WHERE id=999)'):
            with self.subTest(query=query):
                self.strict_error(query)

    def test_rejected_update_delete_leave_rows_unchanged(self):
        self.sql('SET SESSION ai_column_strict_read=ON')
        self.strict_error(f"UPDATE `{self.table}` SET category='changed' WHERE result IS NULL")
        self.strict_error(f'DELETE FROM `{self.table}` WHERE label IS NULL')
        self.assertEqual(self.sql(f'SELECT id,category FROM `{self.table}` ORDER BY id'),
                         ((1,'news'),(2,'sports')))

    def test_user_cannot_assign_ai_results(self):
        for query,args in ((f'UPDATE `{self.table}` SET result=%s WHERE id=1',('forged',)),
                           (f'INSERT INTO `{self.table}`(id,body,result) VALUES (3,%s,%s)',('three','forged'))):
            with self.subTest(query=query),self.assertRaises(pymysql.err.DatabaseError):
                self.sql(query,args)
        self.assertEqual(self.sql(f'SELECT COUNT(*) FROM `{self.table}`'),((2,),))

    def task_snapshot(self):
        rows=self.sql('''SELECT JSON_UNQUOTE(JSON_EXTRACT(task.result_spec,'$.column_name')),
                     task.rowkey_predicate, task.request_id
              FROM oceanbase.__all_ai_column_task task
              JOIN oceanbase.__all_table t ON t.table_id=task.table_id
              WHERE t.table_name=%s''',(self.table,))
        return {(column,predicate): request for column,predicate,request in rows}

    def test_retry_targets_one_column_and_selected_rows_transactionally(self):
        before=self.task_snapshot()
        self.assertEqual(len(before),4)
        self.sql(f'ALTER TABLE `{self.table}` RETRY AI COLUMN result WHERE id IN (1)')
        after=self.task_snapshot()
        self.assertEqual(set(before),set(after))
        changed=[key for key in before if before[key]!=after[key]]
        self.assertEqual(len(changed),1)
        self.assertEqual(changed[0][0],'result')
        self.assertIn('1',changed[0][1])
        self.db.rollback()
        # RETRY must not implicitly commit the INSERT or its replacement task.
        self.assertEqual(self.sql(f'SELECT COUNT(*) FROM `{self.table}`'),((0,),))
        self.assertEqual(self.task_snapshot(),{})

    def ready_baseline(self):
        # Replace setup's uncommitted fixture with real worker-produced READY rows.
        from test_ai_column_worker_integration import RecordingModel
        import time
        self.db.rollback()
        http=RecordingModel()
        self.addCleanup(http.close)
        endpoint=self.table+'_ep'
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
            (endpoint,json.dumps(dict(ai_model_name=self.model,
             url=f'http://127.0.0.1:{http.server.server_port}/v1/chat/completions',
             access_key='local-fixture',provider='openai',request_model_name='fixture'))))
        self.addCleanup(self.sql,'CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)',(endpoint,))
        self.addCleanup(self.db.rollback)
        self.sql(f"INSERT INTO `{self.table}`(id,category,status,body) VALUES "
                 "(1,'news','active','one-ready'),(2,'sports','active','two-ready')")
        deadline=time.monotonic()+30
        while True:
            ready=self.sql(f'SELECT COUNT(*) FROM `{self.table}` WHERE result IS NOT NULL AND label IS NOT NULL')
            if ready==((2,),) and not self.task_snapshot():
                break
            if time.monotonic()>=deadline:
                self.fail('native worker did not produce READY baseline and drain tasks')
            time.sleep(.03)
        return http

    def test_strict_ready_read_and_own_uncommitted_pending_are_isolated(self):
        self.ready_baseline()
        self.sql('SET SESSION ai_column_strict_read=ON')
        # Resolver-time checks must enter/leave the native statement lifecycle,
        # including repeated all-READY scans returning no unfinished row.
        for _ in range(2):
            self.assertEqual(self.sql(f'SELECT COUNT(*) FROM `{self.table}` WHERE label IS NOT NULL'),((2,),))
            self.assertEqual(len(self.sql(f'SELECT result,label FROM `{self.table}` ORDER BY id')),2)
        self.sql('START TRANSACTION')
        self.sql(f'ALTER TABLE `{self.table}` RETRY AI COLUMN result WHERE id=1')
        self.assertEqual(self.sql(f'SELECT COUNT(*) FROM `{self.table}` WHERE label IS NOT NULL'),((2,),))
        # Only our transaction sees this invalidation; an independent proxy read
        # would incorrectly report READY. Filtering to the untouched row must not
        # weaken the full-column check either.
        self.strict_error(f'SELECT result FROM `{self.table}` WHERE id=2')
        self.sql(f"INSERT INTO `{self.table}`(id,body) VALUES (3,'uncommitted-pending')")
        self.strict_error(f'SELECT label FROM `{self.table}` WHERE id=1')
        self.sql('ROLLBACK')
        self.assertEqual(self.sql(f'SELECT COUNT(*) FROM `{self.table}` WHERE result IS NOT NULL AND label IS NOT NULL'),((2,),))
        self.assertEqual(self.task_snapshot(),{})

    def test_retry_rollback_preserves_ready_values_without_http_or_tasks(self):
        import time
        http=self.ready_baseline()
        baseline=self.sql(f'SELECT id,category,result,label FROM `{self.table}` ORDER BY id')
        with http.lock:
            baseline_http=list(http.events)
        self.assertEqual(len(baseline_http),4)
        observer=pymysql.connect(unix_socket=os.environ['SEEKDB_TEST_SOCKET'],user='root',
            database='test',autocommit=True,charset='utf8mb4',read_timeout=30,write_timeout=30)
        self.addCleanup(observer.close)
        def committed_rows():
            with observer.cursor() as cur:
                cur.execute(f'SELECT id,category,result,label FROM `{self.table}` ORDER BY id')
                return cur.fetchall()
        def committed_tasks():
            with observer.cursor() as cur:
                cur.execute('SELECT COUNT(*) FROM oceanbase.__all_ai_column_task task '
                            'JOIN oceanbase.__all_table t ON t.table_id=task.table_id '
                            'WHERE t.table_name=%s',(self.table,))
                return cur.fetchall()
        self.sql('START TRANSACTION')
        self.sql(f"INSERT INTO `{self.table}`(id,category,status,body) VALUES (3,'new','active','three-rollback')")
        self.sql(f"ALTER TABLE `{self.table}` RETRY AI COLUMN result WHERE status='READY' AND id=1")
        self.assertEqual(self.sql(f'SELECT result,label FROM `{self.table}` WHERE id=1'),
                         ((None,baseline[0][3]),))
        self.assertEqual(len(self.task_snapshot()),3)
        # Even rejected mixed ALTER must not commit either the INSERT or RETRY.
        with self.assertRaises(pymysql.err.DatabaseError) as exc:
            self.sql(f'ALTER TABLE `{self.table}` RETRY AI COLUMN result, ADD COLUMN extra INT')
        self.assertEqual(exc.exception.args[0],1235)
        self.assertEqual(committed_rows(),baseline)
        self.assertEqual(committed_tasks(),((0,),))
        time.sleep(.4)
        with http.lock:
            self.assertEqual(http.events,baseline_http,'uncommitted RETRY/INSERT must not reach HTTP')
        self.sql('ROLLBACK')
        self.assertEqual(self.sql(f'SELECT id,category,result,label FROM `{self.table}` ORDER BY id'),baseline)
        self.assertEqual(committed_rows(),baseline)
        self.assertEqual(self.task_snapshot(),{})
        self.assertEqual(committed_tasks(),((0,),))
        time.sleep(.4)
        with http.lock:
            self.assertEqual(http.events,baseline_http,'rollback must leave no request for worker dispatch')

    def test_retry_status_quoted_business_column_and_zero_match(self):
        before=self.task_snapshot()
        self.sql(f"ALTER TABLE `{self.table}` RETRY AI COLUMN result WHERE status='READY'")
        self.assertEqual(self.task_snapshot(),before)
        self.sql(f"ALTER TABLE `{self.table}` RETRY AI COLUMN result WHERE status='PENDING' AND id=999")
        self.assertEqual(self.task_snapshot(),before)
        self.sql(f"ALTER TABLE `{self.table}` RETRY AI COLUMN result WHERE `status`='active' AND id=1")
        after=self.task_snapshot()
        changed=[key for key in before if before[key]!=after[key]]
        self.assertEqual(len(changed),1)
        self.assertEqual(changed[0][0],'result')

    def test_retry_rejects_non_ai_column_and_mixed_actions(self):
        for statement in (f'ALTER TABLE `{self.table}` RETRY AI COLUMN body WHERE id=1',
                          f'ALTER TABLE `{self.table}` RETRY AI COLUMN result WHERE id=1, ADD COLUMN extra INT'):
            with self.subTest(statement=statement),self.assertRaises(pymysql.err.DatabaseError) as exc:
                self.sql(statement)
            self.assertEqual(exc.exception.args[0],1235)


if __name__=='__main__':
    unittest.main()
