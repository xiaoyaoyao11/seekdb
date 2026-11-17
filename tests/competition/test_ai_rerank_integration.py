"""Actual SQL + recording HTTP rerank regressions. Run in server's network namespace.
Requires PyMySQL; SEEKDB_TEST_SOCKET or SEEKDB_TEST_HOST/PORT selects local DB.
No external AI service or credentials. SQL resources are uniquely named/cleaned.
"""
import json
import os
import threading
import unittest
import uuid
from typing import Any
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import pymysql


class AIRerankIntegration(unittest.TestCase):
    def setUp(self):
        self.requests = []
        self.mode = "normal"
        owner = self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, format, *args):
                pass
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                owner.requests.append(body)
                docs = body['documents']
                rows: list[Any] = [{'index': i, 'relevance_score': int(d.split(':')[0])}
                        for i, d in reversed(list(enumerate(docs)))]
                mode = owner.mode
                if mode == 'duplicate':
                    rows[-1]['index'] = rows[0]['index']
                elif mode == 'missing':
                    rows.pop()
                elif mode == 'range':
                    rows[0]['index'] = len(docs)
                elif mode == 'negative':
                    rows[0]['index'] = -1
                elif mode == 'index_type':
                    rows[0]['index'] = '0'
                elif mode == 'fractional_index':
                    rows[0]['index'] = 0.5
                elif mode == 'boolean_index':
                    rows[0]['index'] = True
                elif mode == 'no_index':
                    rows[0].pop('index')
                elif mode == 'extra_row':
                    rows.append(dict(rows[0]))
                elif mode == 'boolean_score':
                    rows[0]['relevance_score'] = True
                elif mode == 'score_type':
                    rows[0]['relevance_score'] = '0.9'
                elif mode == 'null_score':
                    rows[0]['relevance_score'] = None
                elif mode == 'nonfinite':
                    rows[0]['relevance_score'] = float('inf')
                elif mode == 'row_type':
                    rows[0] = 3
                elif mode == 'no_score':
                    rows[0].pop('relevance_score')
                response = {'results': rows}
                if mode == 'results_type':
                    response = {'results': {}}
                elif mode == 'root_type':
                    response = []
                raw = json.dumps(response).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.db = pymysql.connect(unix_socket=os.getenv('SEEKDB_TEST_SOCKET') or None,
            host=os.getenv('SEEKDB_TEST_HOST', '127.0.0.1'),
            port=int(os.getenv('SEEKDB_TEST_PORT', '2886')),
            user=os.getenv('SEEKDB_TEST_USER', 'root'),
            password=os.getenv('SEEKDB_TEST_PASSWORD', ''),
            database=os.getenv('SEEKDB_TEST_DATABASE', 'test'),
            autocommit=True, charset='utf8mb4', read_timeout=30)
        self.addCleanup(self.db.close)
        self.model = 'rr_' + uuid.uuid4().hex
        self.endpoint = self.model + '_ep'
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                 (self.model, json.dumps({'type': 'rerank', 'model_name': 'fixture-reranker'})))
        self.addCleanup(self.sql, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (self.model,))
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                 (self.endpoint, json.dumps({'ai_model_name': self.model, 'scope': 'all',
                   'url': 'http://127.0.0.1:%d/v1/rerank' % self.server.server_port,
                   'access_key': 'local-fixture', 'provider': 'siliconflow'})))
        self.addCleanup(self.sql, 'CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', (self.endpoint,))

    def sql(self, statement, args=()):
        with self.db.cursor() as cursor:
            cursor.execute(statement, args)
            row = cursor.fetchone()
            return row[0] if row else None

    def rerank(self, docs, key=None):
        return json.loads(self.sql('SELECT AI_RERANK(%s,%s,%s,%s)',
                         (self.model, 'fixture query', json.dumps(docs), key)))

    def test_null_fourth_argument_string_array(self):
        result = self.rerank(['2:apple', '1:banana', '3:fruit', '0:vegetable'])
        self.assertEqual([r['index'] for r in result], [2, 0, 1, 3])
        self.assertEqual(self.requests[0]['documents'],
                         ['2:apple', '1:banana', '3:fruit', '0:vegetable'])
        self.assertEqual(self.requests[0]['query'], 'fixture query')
        self.assertEqual(self.requests[0]['model'], 'fixture-reranker')

    def test_three_arguments_native_json_array(self):
        result = json.loads(self.sql(
            "SELECT AI_RERANK(%s,'fixture query',JSON_ARRAY('1:a','2:b'))",
            (self.model,)))
        self.assertEqual([row['index'] for row in result], [1, 0])
        self.assertEqual(self.requests[0]['documents'], ['1:a', '2:b'])

    def test_object_array_preserves_ids_metadata_and_batches(self):
        docs = [{'id': 'original-%d' % i, 'text': '%d:doc-%d' % (i % 7, i),
                 'nested': {'i': i}, '_model_score': -100} for i in range(47)]
        result = self.rerank(docs, 'text')
        expected = sorted(range(len(docs)), key=lambda i: -(i % 7))
        self.assertEqual([r['id'] for r in result], [docs[i]['id'] for i in expected])
        for row, i in zip(result, expected):
            self.assertEqual(row, dict(docs[i], _model_score=i % 7))
        self.assertEqual([len(r['documents']) for r in self.requests], [20, 20, 7])
        self.assertEqual([d for r in self.requests for d in r['documents']],
                         [d['text'] for d in docs])

    def test_string_batches_global_indices_and_stable_ties(self):
        docs = ['%d:doc-%d' % (i % 4, i) for i in range(43)]
        result = self.rerank(docs)
        self.assertEqual([r['index'] for r in result],
                         sorted(range(43), key=lambda i: -(i % 4)))
        self.assertEqual([len(r['documents']) for r in self.requests], [20, 20, 3])

    def test_invalid_provider_responses_are_sql_errors(self):
        for mode in ('duplicate', 'missing', 'range', 'negative', 'index_type',
                     'fractional_index', 'boolean_index', 'no_index', 'extra_row',
                     'boolean_score', 'score_type', 'null_score', 'nonfinite', 'row_type',
                     'no_score', 'results_type', 'root_type'):
            with self.subTest(mode=mode):
                self.mode = mode
                with self.assertRaises(pymysql.err.DatabaseError) as error:
                    self.rerank(['1:a', '0:b'])
                self.assertNotIn(error.exception.args[0], range(2000, 3000))
                self.assertEqual(self.sql('SELECT 1'), 1)

    def test_invalid_documents_do_not_call_provider(self):
        for docs, key in (({}, None), ([], None), ([1], None),
                          ([{'text': None}], 'text'), ([{'other': 'x'}], 'text'),
                          (['1:a', {'text': '0:b'}], None)):
            with self.subTest(docs=docs):
                with self.assertRaises(pymysql.err.DatabaseError):
                    self.rerank(docs, key)
        self.assertEqual(self.requests, [])

    def test_sql_null_inputs_propagate_without_network(self):
        for sql in ('SELECT AI_RERANK(NULL,\'q\',\'["1:a"]\')',
                    'SELECT AI_RERANK(%s,NULL,\'["1:a"]\')',
                    'SELECT AI_RERANK(%s,\'q\',NULL)'):
            self.assertIsNone(self.sql(sql, (self.model,) if '%s' in sql else ()))
        self.assertEqual(self.requests, [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
