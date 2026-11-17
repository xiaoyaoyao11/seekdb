"""Real native SQL/HTTP embedding regressions; run with SEEKDB_TEST_SOCKET.
The loopback recording endpoint returns deterministic test vectors, not model quality.
Run this file INSIDE the server's network namespace (e.g. WSL).
"""
import json
import math
import os
import threading
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pymysql


class AIEmbedIntegration(unittest.TestCase):
    def setUp(self):
        self.db = pymysql.connect(unix_socket=os.environ.get('SEEKDB_TEST_SOCKET'),
            host=os.environ.get('SEEKDB_TEST_HOST', '127.0.0.1'),
            port=int(os.environ.get('SEEKDB_TEST_PORT', '2886')),
            user=os.environ.get('SEEKDB_TEST_USER', 'root'),
            password=os.environ.get('SEEKDB_TEST_PASSWORD', ''), database='test',
            charset='utf8mb4', autocommit=True, read_timeout=60)
        self.addCleanup(self.db.close)
        self.requests = []
        self.mode = 'normal'
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, format, *args):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                owner.requests.append(body)
                inputs = body['input']
                inputs = inputs if isinstance(inputs, list) else [inputs]
                dim = body.get('dimensions', 3)
                data = [{'index': i, 'embedding': owner.vector(s, dim)}
                        for i, s in enumerate(inputs)]
                if owner.mode == 'strings':
                    data[0]['embedding'] = ['bad'] * dim
                elif owner.mode == 'booleans':
                    data[0]['embedding'] = [True] * dim
                elif owner.mode == 'null_element':
                    data[0]['embedding'][0] = None
                elif owner.mode == 'nonfinite':
                    data[0]['embedding'][0] = float('inf')
                elif owner.mode == 'empty_vector':
                    data[0]['embedding'] = []
                elif owner.mode == 'missing_index':
                    del data[0]['index']
                elif owner.mode == 'negative_index':
                    data[0]['index'] = -1
                elif owner.mode == 'short':
                    data = data[:-1]
                elif owner.mode == 'duplicate':
                    data[-1]['index'] = 0
                elif owner.mode == 'wrong_dimension':
                    data[0]['embedding'].append(1)
                elif owner.mode == 'object_data':
                    data = {}
                elif owner.mode == 'http_error':
                    self.send_response(503)
                    self.end_headers()
                    self.wfile.write(b'{"error":"fixture unavailable"}')
                    return
                payload = json.dumps({'data': list(reversed(data)) if isinstance(data, list) else data}).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.model = 'embed_reg_' + uuid.uuid4().hex
        self.endpoint = self.model + '_ep'
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
            (self.model, json.dumps({'type': 'dense_embedding', 'model_name': 'fixture'})))
        self.addCleanup(self.sql, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (self.model,))
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
            (self.endpoint, json.dumps({'ai_model_name': self.model, 'scope': 'all',
             'url': 'http://127.0.0.1:%d/v1/embeddings' % server.server_port,
             'access_key': 'local-fixture-only', 'provider': 'openai'})))
        self.addCleanup(self.sql, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', (self.endpoint,))

    @staticmethod
    def vector(s, dim=3):
        return ([len(s), sum(map(ord, s)) % 101] + [0.25] * max(0, dim - 2))[:dim]

    def sql(self, statement, params=()):
        with self.db.cursor() as cur:
            cur.execute(statement, params)
            return cur.fetchall()

    def error(self, statement, params=()):
        with self.assertRaises(pymysql.err.DatabaseError) as caught:
            self.sql(statement, params)
        self.assertNotIn(caught.exception.args[0], range(2000, 3000), 'transport failure is not semantic RED')
        self.assertEqual(self.sql('SELECT 1'), ((1,),))

    def table(self, count=6):
        name = 'embed_rows_' + uuid.uuid4().hex
        self.sql('CREATE TABLE ' + name + '(id INT PRIMARY KEY, content LONGTEXT)')
        self.addCleanup(self.sql, 'DROP TABLE ' + name)
        for i in range(count):
            self.sql('INSERT INTO ' + name + ' VALUES(%s,%s)', (i, None if i == 2 else 'row-' + str(i)))
        return name

    def test_null_no_http(self):
        self.assertEqual(self.sql('SELECT AI_EMBED(%s,NULL)', (self.model,)), ((None,),))
        self.assertEqual(self.requests, [])

    def test_single_and_lob(self):
        table = self.table(1)
        content = '你好\\"\n' * 10000
        self.sql('UPDATE ' + table + ' SET content=%s', (content,))
        result = self.sql('SELECT AI_EMBED(%s,content,4) FROM ' + table, (self.model,))[0][0]
        self.assertEqual(json.loads(result), self.vector(content, 4))
        self.assertEqual(self.requests[0]['input'], [content])
        self.assertEqual(self.requests[0]['dimensions'], 4)
        self.assertTrue(all(math.isfinite(x) for x in json.loads(result)))

    def test_invalid_dimensions_no_http(self):
        for dimension in ('NULL', '0', '-1', '1.5', "'3'", 'TRUE + 0.5'):
            with self.subTest(dimension=dimension):
                self.error('SELECT AI_EMBED(%s,\'x\',' + dimension + ')', (self.model,))
        self.assertEqual(self.requests, [])

    def test_real_batch_null_order_filter(self):
        table = self.table()
        rows = self.sql('SELECT id,AI_EMBED(%s,content) FROM ' + table + ' WHERE id<>1 ORDER BY id', (self.model,))
        self.assertEqual([(i, None if r is None else json.loads(r)) for i, r in rows],
                         [(i, None if i == 2 else self.vector('row-' + str(i))) for i in (0, 2, 3, 4, 5)])
        inputs = [b['input'] for b in self.requests]
        self.assertTrue(any(len(v) > 1 for v in inputs), inputs)
        self.assertEqual(sorted(s for batch in inputs for s in batch), ['row-0', 'row-3', 'row-4', 'row-5'])

    def test_real_batch_nonnull(self):
        table = self.table()
        rows = self.sql('SELECT id,AI_EMBED(%s,content) FROM ' + table + ' WHERE content IS NOT NULL ORDER BY id', (self.model,))
        self.assertEqual([(i, json.loads(r)) for i, r in rows],
                         [(i, self.vector('row-' + str(i))) for i in (0, 1, 3, 4, 5)])
        self.assertTrue(any(len(b['input']) > 1 for b in self.requests), self.requests)
        self.assertEqual(sum(len(b['input']) for b in self.requests), 5)

    def test_all_null_and_empty_scan_no_http(self):
        table = self.table()
        rows = self.sql('SELECT AI_EMBED(%s,content) FROM ' + table + ' WHERE id=2', (self.model,))
        self.assertEqual(rows, ((None,),))
        self.assertEqual(self.sql('SELECT AI_EMBED(%s,content) FROM ' + table + ' WHERE id<0', (self.model,)), ())
        self.assertEqual(self.requests, [])

    def test_case_skip_rows(self):
        table = self.table()
        rows = self.sql('SELECT id,CASE WHEN id>=3 THEN AI_EMBED(%s,content) ELSE NULL END FROM ' + table + ' ORDER BY id', (self.model,))
        self.assertEqual([(i, None if r is None else json.loads(r)) for i, r in rows],
                         [(i, None if i < 3 else self.vector('row-' + str(i))) for i in range(6)])
        self.assertEqual(sorted(s for b in self.requests for s in b['input']), ['row-3', 'row-4', 'row-5'])

    def test_per_row_dimensions_grouping(self):
        table = self.table()
        rows = self.sql('SELECT id,AI_EMBED(%s,content,3+MOD(id,2)) FROM ' + table + ' WHERE content IS NOT NULL ORDER BY id', (self.model,))
        self.assertEqual([(i, json.loads(r)) for i, r in rows],
                         [(i, self.vector('row-' + str(i), 3+i%2)) for i in (0, 1, 3, 4, 5)])
        self.assertTrue(any(len(b['input']) > 1 for b in self.requests), self.requests)
        for body in self.requests:
            self.assertTrue(all(3 + int(s.split('-')[1]) % 2 == body['dimensions'] for s in body['input']))

    def test_multiple_batches_and_limit(self):
        table = self.table(1100)
        rows = self.sql('SELECT id,AI_EMBED(%s,content) FROM ' + table + ' WHERE id>=100 ORDER BY id LIMIT 900', (self.model,))
        self.assertEqual(len(rows), 900)
        for i, value in rows:
            self.assertEqual(json.loads(value), self.vector('row-' + str(i)))
        self.assertTrue(any(len(b['input']) > 1 for b in self.requests))
        self.assertLess(len(self.requests), 900)

    def test_response_validation(self):
        for mode in ('strings', 'booleans', 'null_element', 'nonfinite', 'empty_vector',
                     'missing_index', 'negative_index', 'short', 'wrong_dimension', 'object_data', 'http_error'):
            with self.subTest(mode=mode):
                self.mode = mode
                self.error('SELECT AI_EMBED(%s,\'hello\',3)', (self.model,))

    def test_duplicate_index_batch_rejected(self):
        table = self.table()
        self.mode = 'duplicate'
        self.error('SELECT AI_EMBED(%s,content) FROM ' + table + ' WHERE content IS NOT NULL', (self.model,))


if __name__ == '__main__':
    unittest.main(verbosity=2)
