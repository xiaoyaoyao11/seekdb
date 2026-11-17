"""Real native-worker SQL tests; run with SEEKDB_TEST_SOCKET on a private DB.
No application worker, SQL rewriting, or fake database is used. HTTP is the only
fixture. Run in the same network namespace as seekdb (normally WSL).
"""
import json
import os
import threading
import time
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pymysql


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
                inputs = body.get('input', [])
                user = next((m['content'] for m in messages if m['role'] == 'user'),
                            inputs[0] if isinstance(inputs, list) and inputs else inputs or '')
                system = next((m['content'] for m in messages if m['role'] == 'system'), '')
                with fixture.lock:
                    attempt = 1 + sum(e['user'] == user for e in fixture.events)
                    fixture.events.append(dict(user=user, system=system, body=body,
                                               auth=self.headers.get('Authorization')))
                if 'slow' in user:
                    time.sleep(1.5)
                fail = 'fail' in user or ('retry' in user and attempt == 1)
                if fail:
                    status, response = 400, {'error': {'message': 'injected'}}
                elif self.path.endswith('/embeddings'):
                    status = 200
                    response = {'data': [{'index': 0, 'embedding':
                                          [10.0, 0.0, 0.0, 0.0] if 'far' in user else [0.0, 0.0, 0.0, 0.0]}]}
                else:
                    value = '7' if 'base-10 integer' in system else (
                        json.dumps({'input': user}) if 'valid JSON value' in system else 'result:' + user)
                    if 'bad-int' in user and 'base-10 integer' in system:
                        value = '7trailing'
                    status, response = 200, {'choices': [{'message': {'content': value}}]}
                data = json.dumps(response).encode()
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                try:
                    self.wfile.write(data)
                except BrokenPipeError:
                    pass

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(5)
        assert not self.thread.is_alive()


class AIColumnWorkerIntegration(unittest.TestCase):
    def setUp(self):
        self.db = pymysql.connect(unix_socket=os.environ['SEEKDB_TEST_SOCKET'], user='root',
                                 database='test', autocommit=True, charset='utf8mb4',
                                 read_timeout=30, write_timeout=30)
        self.addCleanup(self.db.close)
        self.http = RecordingModel()
        self.addCleanup(self.http.close)
        self.name = 'aicw_' + uuid.uuid4().hex[:12]
        self.model, self.endpoint = self.name + '_model', self.name + '_ep'
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                 (self.model, json.dumps({'type': 'completion', 'model_name': 'fixture'})))
        self.addCleanup(self.sql, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (self.model,))
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                 (self.endpoint, json.dumps(dict(ai_model_name=self.model,
                  url=f'http://127.0.0.1:{self.http.server.server_port}/v1/chat/completions',
                  access_key='local-fixture', provider='openai', request_model_name='fixture'))))
        self.addCleanup(self.sql, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', (self.endpoint,))
        self.addCleanup(self.sql, f'DROP TABLE IF EXISTS `{self.name}`')
        self.sql(f'''CREATE TABLE `{self.name}` (id BIGINT PRIMARY KEY, body VARCHAR(200),
          tags JSON AI COLUMN(AI_COMPLETE(%s,CONCAT('json:',body))) WITH(max_retries=2),
          score BIGINT AI COLUMN(AI_COMPLETE(%s,CONCAT('int:',body))) WITH(max_retries=2))''',
                 (self.model, self.model))
        self.table_id = self.sql('SELECT table_id FROM oceanbase.__all_table WHERE table_name=%s',
                                 (self.name,))[0][0]

    def sql(self, query, args=None):
        with self.db.cursor() as cur:
            cur.execute(query, args)
            return cur.fetchall()

    def until(self, predicate, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(.03)
        self.fail('native AI COLUMN worker deadline exceeded')

    def tasks(self):
        return self.sql('SELECT state,retry_count FROM oceanbase.__all_ai_column_task WHERE table_id=%s',
                        (self.table_id,))

    def done(self, row=1):
        return self.sql(f'SELECT tags IS NOT NULL AND score IS NOT NULL FROM `{self.name}` WHERE id=%s',
                        (row,)) == ((1,),)

    def string_table(self, result_type, body_type='LONGTEXT', charset='utf8mb4'):
        self.sql(f'DROP TABLE `{self.name}`')
        self.sql(f'''CREATE TABLE `{self.name}` (id BIGINT PRIMARY KEY, body {body_type},
          answer {result_type} CHARACTER SET {charset}
          AI COLUMN(AI_COMPLETE(%s,body)) WITH(max_retries=0))''', (self.model,))
        self.table_id = self.sql('SELECT table_id FROM oceanbase.__all_table WHERE table_name=%s',
                                 (self.name,))[0][0]

    def column_states(self, columns, row=1):
        # Lock one business row, then inspect public RETRY status predicates.
        # All observations share the row lock; every probe is rolled back, so
        # no new generation or model call can be committed by the observer.
        found=[]
        self.sql('START TRANSACTION')
        try:
            self.assertEqual(self.sql(f'SELECT id FROM `{self.name}` WHERE id=%s FOR UPDATE',(row,)),((row,),))
            with self.db.cursor() as cur:
                for column in columns:
                    for state in ('PENDING','READY','FAILED'):
                        matched=cur.execute(f'ALTER TABLE `{self.name}` RETRY AI COLUMN `{column}` WHERE id=%s AND status=%s',
                                            (row,state))
                        if matched:
                            self.assertEqual(matched,1)
                            found.append(state)
                            break
                    else:
                        self.fail('no valid AI COLUMN status matched')
        finally:
            self.sql('ROLLBACK')
        return tuple(found)

    def assert_answer_state(self, row, expected):
        self.assertEqual(self.column_states(('answer',),row),(expected,))

    def test_accepted_char_and_text_targets_publish_without_truncation(self):
        # CHAR/VARCHAR lengths are characters; TEXT limits are encoded bytes.
        for result_type, good, too_long in (
                ('CHAR(9)', '中😀', '中😀x'),
                ('VARCHAR(9)', '中😀', '中😀x'),
                ('TINYTEXT', '中' * 82, '中' * 84),
                ('TEXT', '中' * 21842, '中' * 21844),
                ('MEDIUMTEXT', '中😀', None),
                ('LONGTEXT', '中😀', None)):
            with self.subTest(result_type=result_type):
                self.string_table(result_type)
                self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', (good,))
                self.until(lambda: not self.tasks())
                self.assert_answer_state(1,'READY')
                self.assertTrue(self.sql(f'SELECT answer FROM `{self.name}` WHERE id=1')
                                == (('result:' + good,),), 'native publication must preserve the full value')
                self.assertTrue(any(e['user'] == good for e in self.http.events))
                if too_long is not None:
                    self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(2,%s)', (too_long,))
                    self.until(lambda: not self.tasks())
                    self.assert_answer_state(2,'FAILED')
                    self.assertEqual(self.sql(f'SELECT answer IS NULL FROM `{self.name}` WHERE id=2'), ((1,),))
                    self.assertTrue(any(e['user'] == too_long for e in self.http.events))

    def test_string_destination_charset_is_validated_before_publication(self):
        # This seekdb baseline supports UTF8MB4 and BINARY only (not latin1).
        # Binary TEXT destinations must enforce encoded bytes, not characters.
        self.string_table('TINYTEXT', charset='binary')
        good,overflow='é' * 120,'é' * 125
        self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s),(2,%s)', (good,overflow))
        self.until(lambda: not self.tasks())
        self.assert_answer_state(1,'READY')
        self.assert_answer_state(2,'FAILED')
        self.assertEqual(self.sql(f'SELECT answer FROM `{self.name}` WHERE id=1'),
                         ((('result:' + good).encode('utf8'),),))
        self.assertEqual(self.sql(f'SELECT answer IS NULL FROM `{self.name}` WHERE id=2'), ((1,),))
        self.assertEqual(sorted(len(e['user']) for e in self.http.events), [len(good),len(overflow)])

    def test_oversized_input_fails_only_its_task_and_small_row_progresses(self):
        self.string_table('TEXT')
        # Server-side construction avoids the client's max_allowed_packet as a confounder.
        # Commit both generations together so the oversized oldest task shares a batch.
        self.sql('START TRANSACTION')
        self.sql(f"INSERT INTO `{self.name}`(id,body) VALUES(1,REPEAT('x',5*1024*1024)),(2,'small')")
        self.assertEqual(self.sql('SELECT OCTET_LENGTH(input) FROM oceanbase.__all_ai_column_task '
                                 'WHERE table_id=%s ORDER BY OCTET_LENGTH(input) DESC',
                                 (self.table_id,)), ((5 * 1024 * 1024,), (5,)))
        self.sql('COMMIT')
        self.until(lambda: not self.tasks())
        self.assert_answer_state(1,'FAILED')
        self.assert_answer_state(2,'READY')
        self.assertEqual(self.sql(f'SELECT id,answer FROM `{self.name}` ORDER BY id'),
                         ((1, None), (2, 'result:small')))
        self.assertEqual([len(e['user']) for e in self.http.events], [5],
                         'oversized input must fail before HTTP, not truncate into another prompt')

    def test_committed_group_is_atomic_and_typed(self):
        self.sql('START TRANSACTION')
        self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', ('group',))
        time.sleep(.4)
        self.assertEqual(self.http.events, [], 'HTTP must not see uncommitted tasks')
        self.assertFalse(self.done())
        self.sql('COMMIT')
        def observe():
            row = self.sql(f'SELECT (tags IS NOT NULL)+(score IS NOT NULL) FROM `{self.name}`')[0][0]
            self.assertNotEqual(row, 1, 'group must publish in one native transaction')
            return row == 2
        self.until(observe)
        self.until(lambda: not self.tasks())
        self.assertEqual(self.sql(f'SELECT score FROM `{self.name}`'), ((7,),))
        self.assertTrue(any('valid JSON value' in e['system'] for e in self.http.events))
        self.assertTrue(any('base-10 integer' in e['system'] for e in self.http.events))
        self.assertEqual({e['user'] for e in self.http.events}, {'json:group', 'int:group'})
        self.assertTrue(all(e['auth'] == 'Bearer local-fixture' for e in self.http.events))

    def test_retry_state_is_committed_before_success(self):
        self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', ('retry',))
        self.until(lambda: (3, 1) in self.tasks())
        self.until(self.done)
        self.until(lambda: not self.tasks())

    def test_stale_response_and_delete_cannot_overwrite(self):
        self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', ('slow-old',))
        self.until(lambda: self.http.events)
        self.sql(f'UPDATE `{self.name}` SET body=%s WHERE id=1', ('new',))
        self.until(self.done)
        time.sleep(2)
        self.assertEqual(json.loads(self.sql(f'SELECT tags FROM `{self.name}`')[0][0])['input'], 'json:new')
        self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(2,%s)', ('slow-delete',))
        self.until(lambda: any('slow-delete' in e['user'] for e in self.http.events))
        self.sql(f'DELETE FROM `{self.name}` WHERE id=2')
        time.sleep(2)
        self.assertEqual(self.sql(f'SELECT COUNT(*) FROM `{self.name}` WHERE id=2'), ((0,),))
        self.until(lambda: not self.tasks())

    def test_composite_native_identity_survives_key_move_and_reuse(self):
        self.sql(f'DROP TABLE `{self.name}`')
        self.sql(f'''CREATE TABLE `{self.name}` (id VARCHAR(100), part BIGINT,
          body VARCHAR(200), tags JSON AI COLUMN(AI_COMPLETE(%s,body)) WITH(max_retries=0),
          PRIMARY KEY(id,part))''', (self.model,))
        self.table_id = self.sql('SELECT table_id FROM oceanbase.__all_table WHERE table_name=%s',
                                 (self.name,))[0][0]
        old, moved = "key'\\中", "moved'\\中"
        self.sql(f'INSERT INTO `{self.name}`(id,part,body) VALUES(%s,1,%s)', (old, 'slow-old'))
        self.until(lambda: self.http.events)
        self.sql(f'UPDATE `{self.name}` SET id=%s,part=2,body=%s WHERE id=%s AND part=1',
                 (moved, 'new', old))
        self.sql(f'INSERT INTO `{self.name}`(id,part,body) VALUES(%s,1,%s)', (old, 'replacement'))
        self.until(lambda: self.sql(f'SELECT COUNT(*) FROM `{self.name}` WHERE tags IS NOT NULL') == ((2,),))
        time.sleep(2)
        self.until(lambda: not self.tasks())
        for key, part, expected in ((old, 1, 'replacement'), (moved, 2, 'new')):
            value = self.sql(f'SELECT tags FROM `{self.name}` WHERE id=%s AND part=%s', (key, part))[0][0]
            self.assertEqual(json.loads(value)['input'], expected)

    def test_sql_null_input_is_not_replaced_with_empty_prompt(self):
        self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,NULL)')
        self.until(lambda: not self.tasks())
        self.assertEqual(self.http.events, [], 'SQL NULL input must never become an empty-string HTTP prompt')
        self.assertEqual(self.sql(f'SELECT tags IS NULL AND score IS NULL FROM `{self.name}`'), ((1,),))

    def test_rollback_never_reaches_provider(self):
        self.sql('START TRANSACTION')
        self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', ('rolled-back',))
        self.sql('ROLLBACK')
        time.sleep(.6)
        self.assertEqual(self.http.events, [])
        self.assertEqual(self.tasks(), ())

    def test_superseded_group_member_does_not_strand_unchanged_member(self):
        self.sql(f'DROP TABLE `{self.name}`')
        self.sql(f'''CREATE TABLE `{self.name}` (id BIGINT PRIMARY KEY,
          body VARCHAR(200), other VARCHAR(200),
          tags JSON AI COLUMN(AI_COMPLETE(%s,body)) WITH(max_retries=0),
          score BIGINT AI COLUMN(AI_COMPLETE(%s,other)) WITH(max_retries=0))''',
                 (self.model, self.model))
        self.table_id = self.sql('SELECT table_id FROM oceanbase.__all_table WHERE table_name=%s',
                                 (self.name,))[0][0]
        self.sql(f'INSERT INTO `{self.name}`(id,body,other) VALUES(1,%s,%s)',
                 ('slow-old-tags', 'slow-unchanged-score'))
        self.until(lambda: self.http.events)
        self.sql(f'UPDATE `{self.name}` SET body=%s WHERE id=1', ('new-tags',))
        self.until(self.done)
        self.until(lambda: not self.tasks())
        self.assertEqual(self.sql(f'SELECT score FROM `{self.name}`'), ((7,),))
        self.assertEqual(json.loads(self.sql(f'SELECT tags FROM `{self.name}`')[0][0])['input'], 'new-tags')

    def test_exhausted_expired_lease_does_not_call_provider_again(self):
        self.sql('START TRANSACTION')
        self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', ('exhausted',))
        self.sql('''UPDATE oceanbase.__all_ai_column_task SET state=1,retry_count=2,
                    lease_token='dead-owner',lease_expiry=1 WHERE table_id=%s''', (self.table_id,))
        self.sql('COMMIT')
        self.until(lambda: not self.tasks())
        self.assertEqual(self.http.events, [])
        self.assertEqual(self.column_states(('tags','score')),('FAILED','FAILED'))

    def test_success_and_failure_publish_hidden_states_together(self):
        self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', ('bad-int',))
        def observe():
            tags, score = self.column_states(('tags','score'))
            self.assertEqual(tags == 'PENDING', score == 'PENDING', 'success/failure group must publish atomically')
            return tags == 'READY' and score == 'FAILED'
        self.until(observe, timeout=40)
        self.until(lambda: not self.tasks())

    def test_native_vector_publication_updates_ann_and_rollback_restores_index(self):
        model, endpoint = self.name + '_embed', self.name + '_embed_ep'
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                 (model, json.dumps({'type': 'dense_embedding', 'model_name': 'fixture-embed'})))
        self.addCleanup(self.sql, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (model,))
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                 (endpoint, json.dumps(dict(ai_model_name=model,
                  url=f'http://127.0.0.1:{self.http.server.server_port}/v1/embeddings',
                  access_key='local-fixture', provider='openai', request_model_name='fixture-embed'))))
        self.addCleanup(self.sql, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', (endpoint,))
        self.sql(f'DROP TABLE `{self.name}`')
        self.sql(f'''CREATE TABLE `{self.name}` (id BIGINT PRIMARY KEY, body VARCHAR(200),
                     emb VECTOR(4) AI COLUMN(AI_EMBED(%s,body)) WITH(max_retries=0))''', (model,))
        self.addCleanup(self.sql, f'DROP TABLE IF EXISTS `{self.name}`')
        self.table_id = self.sql('SELECT table_id FROM oceanbase.__all_table WHERE table_name=%s',
                                 (self.name,))[0][0]
        self.sql(f"INSERT INTO `{self.name}`(id,body) VALUES(1,'near'),(2,'far')")
        self.until(lambda: self.sql(f'SELECT COUNT(*) FROM `{self.name}` WHERE emb IS NOT NULL') == ((2,),))
        self.sql(f'''CREATE VECTOR INDEX idx_emb ON `{self.name}`(emb)
                     WITH(distance=l2,type=hnsw,lib=vsag,m=10,ef_construction=12,ef_search=40)''')
        ann = f'SELECT id FROM `{self.name}` ORDER BY l2_distance(emb,[10,0,0,0]) APPROXIMATE LIMIT 1'
        self.assertEqual(self.sql(ann), ((2,),))
        self.sql('START TRANSACTION')
        self.sql(f"UPDATE `{self.name}` SET body='near' WHERE id=2")
        self.assertEqual(self.sql(f'SELECT emb IS NULL FROM `{self.name}` WHERE id=2'), ((1,),))
        self.assertEqual(self.sql(ann), ((1,),), 'ordinary DML must remove invalidated vector')
        self.sql('ROLLBACK')
        self.assertEqual(self.sql(ann), ((2,),))
        self.sql(f"UPDATE `{self.name}` SET body='near' WHERE id=2")
        self.until(lambda: self.sql(f'SELECT emb=[0,0,0,0] FROM `{self.name}` WHERE id=2') == ((1,),))
        self.until(lambda: not self.tasks())
        self.assertEqual(set(self.sql(f'''SELECT id FROM `{self.name}`
                            ORDER BY l2_distance(emb,[0,0,0,0]) APPROXIMATE LIMIT 2''')), {(1,), (2,)})
        self.assertTrue(any(e['body'].get('input') for e in self.http.events))

    def test_expired_lease_is_recovered_without_http_transaction(self):
        # Commit an already-leased task while its owner is absent: persistent restart recovery.
        self.sql('START TRANSACTION')
        self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', ('recover',))
        self.sql('''UPDATE oceanbase.__all_ai_column_task SET state=1,
                    lease_token='dead-owner',lease_expiry=1 WHERE table_id=%s''', (self.table_id,))
        self.sql('COMMIT')
        self.until(self.done)
        self.until(lambda: not self.tasks())

    def test_bad_integer_and_provider_failure_are_terminal_null(self):
        for value in ('bad-int', 'fail'):
            self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)', (value,))
            self.until(lambda: self.http.events)
            self.until(lambda: not self.tasks(), timeout=40)
            self.assertEqual(self.sql(f'SELECT score IS NULL FROM `{self.name}`'), ((1,),))
            self.assertEqual(self.column_states(('score',)),('FAILED',))
            self.sql(f'DELETE FROM `{self.name}`')


import pathlib
import shutil
import subprocess
import tempfile
import textwrap


class NativeWorkerValueTests(unittest.TestCase):
    def test_actual_worker_value_helpers(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        scratch = pathlib.Path('/mnt/f/Hermes-Official/cache/scratch')
        directory = str(scratch) if scratch.exists() else None
        source = r'''
          #include "observer/ai_service/ai_column_worker_value.h"
          #include <cassert>
          #include <iostream>
          using namespace oceanbase::observer::ai_column_value;
          int main() {
            int64_t v = 0;
            assert(integer("-9223372036854775808",v) && v == INT64_MIN);
            assert(integer("9223372036854775807",v) && v == INT64_MAX);
            for (auto s : {"", "9223372036854775808", "-9223372036854775809", "1x", "1.0", "1e2", " 1", "--1", "+-1"}) assert(!integer(s,v));
            assert(vector("[0,-1.5,2e-3,4]",4));
            for (auto s : {"[]", "[1]", "[1,2,3,4,5]", "[NaN,0,0,0]", "[null,0,0,0]", "[1e309,0,0,0]", "[3.5e38,0,0,0]", "[01,0,0,0]", "[+1,0,0,0]", "[1.,0,0,0]", "[1,2,3,4,]", "[1,2,3,4]trailing"}) assert(!vector(s,4));
            size_t count = 0;
            assert(utf8_length("a\xe4\xb8\xad\xf0\x9f\x98\x80",count) && count == 3);
            for (auto s : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80", "\xe4\xb8"}) assert(!utf8_length(s,count));
            std::vector<int64_t> ids;
            assert(membership("[16, 17]",ids) && ids.size() == 2);
            for (auto s : {"[]", "[1,1]", "[0]", "[01]", "[1,02]", "[1,]", "[1]junk"}) { ids.clear(); assert(!membership(s,ids)); }
            assert(backoff_us(1) == 1000000 && backoff_us(2) == 2000000 && backoff_us(100) == 32000000);
            std::cout << "actual_worker_value_helpers=PASS\n";
          }
        '''
        with tempfile.TemporaryDirectory(prefix='aicw-native-', dir=directory) as tmp:
            cpp, binary = pathlib.Path(tmp) / 'test.cpp', pathlib.Path(tmp) / 'test'
            cpp.write_text(textwrap.dedent(source))
            compiler = os.environ.get('CXX', shutil.which('g++') or 'clang++')
            subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(root / 'src'), str(cpp), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('actual_worker_value_helpers=PASS', result.stdout)
            print(result.stdout, end='')


class NativeWorkerSQLExpressionTests(unittest.TestCase):
    def test_vector_and_json_casts_use_real_sql_engine(self):
        with pymysql.connect(unix_socket=os.environ['SEEKDB_TEST_SOCKET'], user='root',
                             database='test', autocommit=True) as db:
            with db.cursor() as cur:
                cur.execute("SELECT [0,1,2,3], "
                            "CAST(CONVERT(X'7b2261223a317d' USING utf8mb4) AS JSON)")
                row = cur.fetchone()
                self.assertEqual(json.loads(row[0]), [0, 1, 2, 3])
                self.assertEqual(json.loads(row[1]), {'a': 1})


if __name__ == '__main__':
    unittest.main()
