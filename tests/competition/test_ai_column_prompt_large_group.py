"""Native SQL + recording HTTP regressions for prompt kinds and large groups.

Run in the server's network namespace with SEEKDB_TEST_SOCKET. The parent owns
server startup/builds. No worker replacement, increased SQL cap or real API key.
"""
import json
import os
import threading
import time
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pymysql


SIZE = 4 * 1024 * 1024


class RecordingModel:
    def __init__(self):
        self.events = []
        self.lock = threading.Lock()
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                messages = body.get('messages', [])
                user = next((m['content'] for m in messages if m['role'] == 'user'), None)
                system = next((m['content'] for m in messages if m['role'] == 'system'), '')
                with fixture.lock:
                    fixture.events.append({'user': user, 'system': system, 'body': body})
                if user is not None and user.startswith('large:'):
                    value = user[-1] * SIZE
                elif 'base-10 integer' in system:
                    value = '7'
                elif 'valid JSON value' in system:
                    value = json.dumps({'input': user})
                else:
                    value = 'result:' + str(user)
                data = json.dumps({'choices': [{'message': {'content': value}}]}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                try:
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(5)
        assert not self.thread.is_alive()


class PromptAndLargeGroupTests(unittest.TestCase):
    def setUp(self):
        self.db = pymysql.connect(unix_socket=os.environ['SEEKDB_TEST_SOCKET'], user='root',
                                 database='test', autocommit=True, charset='utf8mb4',
                                 read_timeout=60, write_timeout=60)
        self.addCleanup(self.db.close)
        self.http = RecordingModel()
        self.addCleanup(self.http.close)
        self.name = 'aicpl_' + uuid.uuid4().hex[:12]
        self.model, self.endpoint = self.name + '_m', self.name + '_ep'
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                 (self.model, json.dumps({'type': 'completion', 'model_name': 'fixture'})))
        self.addCleanup(self.sql, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (self.model,))
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                 (self.endpoint, json.dumps(dict(ai_model_name=self.model,
                  url=f'http://127.0.0.1:{self.http.server.server_port}/v1/chat/completions',
                  access_key='local-fixture', provider='openai', request_model_name='fixture'))))
        self.addCleanup(self.sql, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', (self.endpoint,))

    def sql(self, query, args=None):
        with self.db.cursor() as cur:
            cur.execute(query, args)
            return cur.fetchall()

    def create(self, columns):
        name = self.name + '_' + str(len(getattr(self, 'tables', [])))
        self.tables = getattr(self, 'tables', []) + [name]
        self.sql(f'CREATE TABLE `{name}` (id BIGINT PRIMARY KEY, body LONGTEXT,{columns})')
        self.addCleanup(self.sql, f'DROP TABLE IF EXISTS `{name}`')
        return name, self.sql('SELECT table_id FROM oceanbase.__all_table WHERE table_name=%s', (name,))[0][0]

    def until(self, predicate, timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = predicate()
            if result:
                return result
            time.sleep(.05)
        self.fail('native worker deadline exceeded: ' + repr(self.http.events))

    def tasks(self, table):
        return self.sql('SELECT state FROM oceanbase.__all_ai_column_task WHERE table_id=%s', (table,))

    def test_native_prompt_text_json_and_bigint_render_exact_user_messages(self):
        # COALESCE prevents shortcuts based on the top-level function name.
        # Use the supported deterministic-expression gate and a scalar argument;
        # JSON_ARRAY(body) would intentionally render a whole array argument.
        prompt = "COALESCE(AI_PROMPT('hello {0} / {0}',body),AI_PROMPT('unused',body))"
        columns = ','.join(f"`{name}` {kind} AI COLUMN(AI_COMPLETE('{self.model}',{prompt})) WITH(max_retries=0)"
                           for name, kind in [('txt', 'LONGTEXT'), ('doc', 'JSON'), ('num', 'BIGINT')])
        table, tid = self.create(columns)
        self.sql(f'INSERT INTO `{table}`(id,body) VALUES(1,%s)', ('中 {1} "quoted"',))
        self.until(lambda: not self.tasks(tid))
        expected = 'hello 中 {1} "quoted" / 中 {1} "quoted"'
        self.assertEqual(len(self.http.events), 3)
        self.assertEqual([e['user'] for e in self.http.events], [expected] * 3)
        self.assertEqual(self.sql(f'SELECT txt,num FROM `{table}`'), (('result:' + expected, 7),))
        self.assertEqual(json.loads(self.sql(f'SELECT doc FROM `{table}`')[0][0]), {'input': expected})
        self.assertEqual(sorted(e['system'] for e in self.http.events),
                         ['', 'Return exactly one base-10 integer', 'Return exactly one valid JSON value'])

    def test_json_looking_varchar_is_literal_even_for_typed_overrides(self):
        columns = ','.join(f"`{name}` {kind} AI COLUMN(AI_COMPLETE('{self.model}',CONCAT('',body))) WITH(max_retries=0)"
                           for name, kind in [('txt', 'LONGTEXT'), ('doc', 'JSON'), ('num', 'BIGINT')])
        table, tid = self.create(columns)
        literal = '{"template":"not substituted {0}","args":["literal"]}'
        self.sql(f'INSERT INTO `{table}`(id,body) VALUES(1,%s)', (literal,))
        self.until(lambda: not self.tasks(tid))
        self.assertEqual(len(self.http.events), 3)
        self.assertEqual([e['user'] for e in self.http.events], [literal] * 3)
        self.assertEqual(self.sql(f'SELECT txt,num FROM `{table}`'), (('result:' + literal, 7),))
        self.assertEqual(json.loads(self.sql(f'SELECT doc FROM `{table}`')[0][0]), {'input': literal})

    def large_table(self):
        return self.create(','.join(f"c{i} LONGTEXT AI COLUMN(AI_COMPLETE('{self.model}',CONCAT(body,':{i}'))) WITH(max_retries=0)"
                                    for i in range(5)))

    def assert_large_published(self, table, tid):
        def observe():
            row = self.sql(f"SELECT {'+'.join(f'(c{i} IS NOT NULL)' for i in range(5))} FROM `{table}` WHERE id=1")[0][0]
            self.assertIn(row, (0, 5), 'group must never expose a partial commit')
            return row == 5
        self.until(observe)
        self.until(lambda: not self.tasks(tid))
        # Server-side checks avoid transporting aggregate 20 MiB through the
        # Python client and prove all bytes, not just length/prefix.
        checks = ','.join(f"OCTET_LENGTH(c{i})={SIZE} AND c{i}=REPEAT('{i}',{SIZE})" for i in range(5))
        self.assertEqual(self.sql(f'SELECT {checks} FROM `{table}` WHERE id=1'), ((1,) * 5,))

    def test_five_4mib_results_do_not_poison_scheduler_and_small_row_progresses(self):
        table, tid = self.large_table()
        small, sid = self.create(f"answer LONGTEXT AI COLUMN(AI_COMPLETE('{self.model}',body)) WITH(max_retries=0)")
        self.sql(f"INSERT INTO `{table}`(id,body) VALUES(1,'large')")
        # After four staged responses the old terminal metadata scan exceeds
        # 32 MiB; a separate table/row must still reach inference/publication.
        self.until(lambda: len([e for e in self.http.events if e['user'].startswith('large:')]) >= 4)
        self.sql(f"INSERT INTO `{small}`(id,body) VALUES(1,'unrelated-small')")
        self.until(lambda: self.sql(f'SELECT answer FROM `{small}`') == (('result:unrelated-small',),))
        self.until(lambda: not self.tasks(sid))
        self.assert_large_published(table, tid)
        self.assertEqual(sorted(e['user'] for e in self.http.events),
                         [f'large:{i}' for i in range(5)] + ['unrelated-small'])

    def test_recover_five_already_staged_4mib_results_without_inference(self):
        table, tid = self.large_table()
        # Persist a crash-boundary state through the real catalog in the
        # enqueueing transaction; worker never observes an intermediate setup.
        self.sql('START TRANSACTION')
        try:
            self.sql(f"INSERT INTO `{table}`(id,body) VALUES(1,'recover-no-http')")
            ids = self.sql('SELECT column_id FROM oceanbase.__all_ai_column_task WHERE table_id=%s ORDER BY column_id', (tid,))
            self.assertEqual(len(ids), 5)
            for i, (column,) in enumerate(ids):
                self.sql("UPDATE oceanbase.__all_ai_column_task SET state=2,staged_result=REPEAT(%s,%s),lease_token='crashed-owner' WHERE table_id=%s AND column_id=%s",
                         (str(i), SIZE, tid, column))
            self.sql('COMMIT')
        except BaseException:
            self.sql('ROLLBACK')
            raise
        self.assert_large_published(table, tid)
        self.assertEqual(self.http.events, [], 'terminal recovery must not reinfer')


if __name__ == '__main__':
    unittest.main()
