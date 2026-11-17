"""Real seekdb SQL + loopback native Ollama protocol regressions.

Run inside the same network namespace as seekdb, with PyMySQL installed:
  SEEKDB_TEST_SOCKET=/private/run/sql.sock python test_ai_ollama_integration.py -v
Uses SEEKDB_TEST_{HOST,PORT,USER,PASSWORD,DATABASE}; defaults root/test/2886.
No real model/key is used. UUID resources are cleaned up; no official test edits.
A cached baseline run establishes RED only, not validation of changed C++.
"""
import json
import os
import threading
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pymysql


class OllamaIntegration(unittest.TestCase):
    def setUp(self):
        self.db = pymysql.connect(
            host=os.environ.get("SEEKDB_TEST_HOST", "127.0.0.1"),
            unix_socket=os.environ.get("SEEKDB_TEST_SOCKET") or None,
            port=int(os.environ.get("SEEKDB_TEST_PORT", "2886")),
            user=os.environ.get("SEEKDB_TEST_USER", "root"),
            password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
            database=os.environ.get("SEEKDB_TEST_DATABASE", "test"),
            charset="utf8mb4", autocommit=True, connect_timeout=10,
            read_timeout=30, write_timeout=30)
        self.addCleanup(self.db.close)
        self.requests = []
        self.response_override = None
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, format, *args):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                owner.requests.append((self.path, dict(self.headers), body))
                if owner.response_override is not None:
                    response = owner.response_override
                elif self.path == "/api/generate":
                    response = {"response": "native:" + body.get("prompt", ""), "done": True}
                elif self.path == "/api/embed":
                    inputs = body.get("input", [])
                    response = {"embeddings": [[len(text), 0.25, -1.5] for text in inputs]}
                else:
                    self.send_error(404)
                    return
                data = json.dumps(response).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(thread.join, 5)
        self.addCleanup(self.server.shutdown)
        self.base_url = f"http://127.0.0.1:{self.server.server_port}"

    def sql(self, sql, args=()):
        with self.db.cursor() as cur:
            cur.execute(sql, args)
            return cur.fetchall()

    def scalar(self, sql, args=()):
        rows = self.sql(sql, args)
        self.assertEqual(len(rows), 1)
        self.assertEqual(len(rows[0]), 1)
        return rows[0][0]

    def resource(self, kind="completion", provider="ollama", key=""):
        name = "oll_" + uuid.uuid4().hex
        ep = name + "_ep"
        self.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)",
                 (name, json.dumps({"type": kind, "model_name": name + "_native"})))
        # LIFO cleanup drops model before endpoint, matching the official suite.
        self.addCleanup(self.drop_if_exists, "DROP_AI_MODEL_ENDPOINT", ep)
        self.addCleanup(self.drop_if_exists, "DROP_AI_MODEL", name)
        self.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)",
                 (ep, json.dumps({"ai_model_name": name, "scope": "all",
                                 "url": self.base_url + ("/api/generate" if kind == "completion" else "/api/embed"),
                                 "access_key": key, "provider": provider})))
        return name, ep

    def drop_if_exists(self, procedure, name):
        try:
            self.sql(f"CALL DBMS_AI_SERVICE.{procedure}(%s)", (name,))
        except pymysql.err.DatabaseError as exc:
            if exc.args[0] not in (11112, 11118):
                raise

    def provider(self, ep):
        return self.scalar("SELECT provider FROM oceanbase.DBA_OB_AI_MODEL_ENDPOINTS WHERE endpoint_name=%s", (ep,))

    def alter(self, ep, delta):
        self.sql("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)", (ep, json.dumps(delta)))

    def assert_sql_rejected(self, callback):
        with self.assertRaises(pymysql.err.DatabaseError) as caught:
            callback()
        self.assertNotIn(caught.exception.args[0], range(2000, 3000), "transport failure is not SQL rejection")
        self.assertEqual(self.scalar("SELECT 1"), 1)

    def test_endpoint_crud_preserves_provider_case_and_empty_key(self):
        for spelling in ("ollama", "Ollama", "OLLAMA", "oLlAmA"):
            with self.subTest(provider=spelling):
                _, ep = self.resource(provider=spelling)
                self.assertEqual(self.provider(ep), spelling)
                self.alter(ep, {"provider": "Ollama"})
                self.assertEqual(self.provider(ep), "Ollama")
                self.sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)", (ep,))
                self.assertEqual(self.scalar("SELECT count(*) FROM oceanbase.DBA_OB_AI_MODEL_ENDPOINTS WHERE endpoint_name=%s", (ep,)), 0)

    def test_inference_after_provider_only_alter(self):
        for kind in ("completion", "dense_embedding"):
            with self.subTest(kind=kind):
                name, ep = self.resource(kind=kind)
                self.alter(ep, {"provider": "Ollama"})
                if kind == "completion":
                    self.assertEqual(self.scalar("SELECT AI_COMPLETE(%s,%s)",
                                                 (name, "after-alter")), "native:after-alter")
                else:
                    self.assertEqual(json.loads(self.scalar("SELECT AI_EMBED(%s,%s)",
                                                           (name, "after-alter"))),
                                     [len("after-alter"), 0.25, -1.5])
                self.assertEqual(self.provider(ep), "Ollama")

    def test_ollama_with_nonempty_key_is_registered(self):
        _, ep = self.resource(provider="oLlAmA", key="synthetic-key")
        self.assertEqual(self.provider(ep), "oLlAmA")

    def test_non_ollama_providers_still_require_key(self):
        for provider in ("openai", "ALIYUN-OPENAI", "siliconflow", "ALIYUN-DASHSCOPE"):
            with self.subTest(provider=provider):
                self.assert_sql_rejected(lambda: self.resource(provider=provider))

    def test_embed_requested_dimension_is_enforced(self):
        name, _ = self.resource(kind="dense_embedding")
        text = "dimension-input"
        value = self.scalar("SELECT AI_EMBED(%s,%s,3)", (name, text))
        self.assertEqual(json.loads(value), [len(text), 0.25, -1.5])
        self.assertEqual(self.requests[-1][2]["dimensions"], 3)
        self.assert_sql_rejected(lambda: self.scalar("SELECT AI_EMBED(%s,%s,2)", (name, text)))

    def test_openai_empty_key_still_rejected_and_alter_is_atomic(self):
        self.assert_sql_rejected(lambda: self.resource(provider="openai"))
        _, ep = self.resource()
        self.assert_sql_rejected(lambda: self.alter(ep, {"provider": "openai"}))
        self.assertEqual(self.provider(ep), "ollama")
        self.alter(ep, {"provider": "openai", "access_key": "synthetic-key"})
        self.assertEqual(self.provider(ep), "openai")
        self.assert_sql_rejected(lambda: self.alter(ep, {"access_key": ""}))
        self.alter(ep, {"provider": "oLlAmA", "access_key": ""})
        self.assertEqual(self.provider(ep), "oLlAmA")
        self.assert_sql_rejected(lambda: self.alter(ep, {"provider": "not-a-provider"}))
        self.assertEqual(self.provider(ep), "oLlAmA")

    def test_generate_uses_native_nonstreaming_request(self):
        name, _ = self.resource(provider="oLlAmA")
        text = 'arbitrary 中文 "value"\\\n'
        value = self.scalar("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,%s,%s))",
                            (name, text, json.dumps({"stream": True, "options": {"temperature": 0.2}})))
        self.assertEqual(value, "native:" + text)
        path, headers, body = self.requests[-1]
        self.assertEqual(path, "/api/generate")
        self.assertEqual(body["model"], name + "_native")
        self.assertEqual(body["prompt"], text)
        self.assertIs(body["stream"], False)
        self.assertEqual(body["options"], {"temperature": 0.2})
        self.assertNotIn("messages", body)
        self.assertEqual(headers.get("Content-Type"), "application/json")
        self.assertNotIn("Authorization", headers)

    def test_native_embed_numeric_vector_and_array_input(self):
        name, _ = self.resource(kind="dense_embedding", provider="OLLAMA")
        text = "unique-input"
        result = json.loads(self.scalar("SELECT AI_EMBED(%s,%s)", (name, text)))
        self.assertEqual(result, [len(text), 0.25, -1.5])
        path, headers, body = self.requests[-1]
        self.assertEqual(path, "/api/embed")
        self.assertEqual(body["model"], name + "_native")
        self.assertEqual(body["input"], [text])
        self.assertNotIn("Authorization", headers)
        self.assertEqual(headers.get("Content-Type"), "application/json")

    def test_generate_rejects_nonstring_or_missing_response(self):
        name, _ = self.resource()
        for response in ({}, {"response": 123}, {"response": None}, {"response": {"x": 1}}):
            with self.subTest(response=response):
                self.response_override = response
                self.assert_sql_rejected(lambda: self.scalar("SELECT AI_COMPLETE(%s,'x')", (name,)))

    def test_embed_rejects_malformed_native_vectors(self):
        name, _ = self.resource(kind="dense_embedding")
        for response in ({}, {"embeddings": "bad"}, {"embeddings": [1]},
                         {"embeddings": [["bad", 1]]}, {"embeddings": [[None]]},
                         {"embeddings": [[]]}, {"embeddings": []}):
            with self.subTest(response=response):
                self.response_override = response
                self.assert_sql_rejected(lambda: self.scalar("SELECT AI_EMBED(%s,'x')", (name,)))

    def test_embed_rejects_wrong_response_count(self):
        name, _ = self.resource(kind="dense_embedding")
        self.response_override = {"embeddings": [[1, 2, 3], [4, 5, 6]]}
        self.assert_sql_rejected(lambda: self.scalar("SELECT AI_EMBED(%s,'x')", (name,)))


if __name__ == "__main__":
    unittest.main()
