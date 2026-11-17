"""Local native AI_COMPLETE(AI_PROMPT(...)) rendering regressions.

Requires PyMySQL, a running seekdb, and CREATE/DROP AI MODEL privileges.
Run this process in the SAME network namespace as seekdb (e.g. inside WSL):
    python tests/competition/test_ai_prompt_rendering.py -v
Before production edits, preserve the UNMODIFIED baseline result with:
    python tests/competition/test_ai_prompt_rendering.py \
        AIPromptRendering.test_repeat_long_string -v
Connection/import/registration failures are blockers, NOT regression evidence.

Uses SEEKDB_TEST_HOST=127.0.0.1, SEEKDB_TEST_PORT=2886,
SEEKDB_TEST_USER=root, SEEKDB_TEST_DATABASE=test, SEEKDB_TEST_PASSWORD=""
Also supports SEEKDB_TEST_SOCKET for a Unix socket with SQL TCP disabled.
These match test_ai_prompt_integration.py. Use a local test database.
The recording listener binds only 127.0.0.1 on an ephemeral port. It returns a
fixed protocol fixture, never calls a real model, and uses only the public
mock-key credential. This is focused local regression apparatus, NOT the
competition's required official MockAI service or evidence of official scoring.

Source contracts: ob_expr_ai_complete.cpp defers rendering; ob_ai_func_utils.cpp
builds messages[].content and parses choices[0].message.content, returning SQL
text (not an extra JSON-quoted string). DBMS_AI_SERVICE's create procedures take (name, params JSON), and
drop procedures take (name). Baseline rejects structured rendering and sizes its
string buffer using each argument only once; acceptance tests must expose these.
"""

import json
import os
import queue
import threading
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, HTTPServer
from typing import cast

import pymysql


PATH = "/v1/chat/completions"
REPLY = "recording-fixture-response"


class RecordingServer(HTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), RecordingHandler)
        self.requests = queue.Queue()
        self.response: object = {"choices": [{"message": {"role": "assistant", "content": REPLY}}]}
        self.response_status = 200


class RecordingHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        self.connection.settimeout(10)
        raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        # Record before replying so the synchronous SQL call cannot outrun us.
        # Do not retain/log Authorization or database credentials.
        cast(RecordingServer, self.server).requests.put((self.path, raw))
        payload = json.dumps(cast(RecordingServer, self.server).response).encode("utf-8")
        self.send_response(cast(RecordingServer, self.server).response_status if self.path == PATH else 404)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format, *args):
        pass


class AIPromptRendering(unittest.TestCase):
    def setUp(self):
        self.connection = pymysql.connect(
            unix_socket=os.environ.get("SEEKDB_TEST_SOCKET") or None,
            host=os.environ.get("SEEKDB_TEST_HOST", "127.0.0.1"),
            port=int(os.environ.get("SEEKDB_TEST_PORT", "2886")),
            user=os.environ.get("SEEKDB_TEST_USER", "root"),
            password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
            database=os.environ.get("SEEKDB_TEST_DATABASE", "test"),
            charset="utf8mb4", autocommit=True,
            connect_timeout=10, read_timeout=30, write_timeout=30,
        )
        self.addCleanup(self.connection.close)
        self.server = RecordingServer()
        self.addCleanup(self.server.server_close)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop_server)
        suffix = uuid.uuid4().hex
        self.model = "prompt_render_model_" + suffix
        self.endpoint = "prompt_render_endpoint_" + suffix
        self.request_model = "recording_" + suffix
        self.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s, %s)", (
            self.model, json.dumps({"type": "completion",
                                   "model_name": self.request_model})))
        # Register cleanup ONLY after the corresponding create succeeded.
        self.addCleanup(self.execute, "CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)",
                        (self.model,))
        port = self.server.server_address[1]
        self.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s, %s)", (
            self.endpoint, json.dumps({
                "ai_model_name": self.model, "scope": "all",
                "url": f"http://127.0.0.1:{port}{PATH}",
                "access_key": "mock-key", "provider": "openai",
                "request_model_name": self.request_model,
            })))
        self.addCleanup(self.execute,
                        "CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)",
                        (self.endpoint,))
        # Prove the fixture/provider works before negative tests can pass.
        result = self.scalar("SELECT AI_COMPLETE(%s, %s)", (self.model, "probe"))
        self.assertEqual(result, REPLY)
        self.assertEqual(self.captured_prompt(), "probe")

    def stop_server(self):
        self.server.shutdown()
        self.thread.join(timeout=10)
        self.assertFalse(self.thread.is_alive(), "Recording listener did not stop")

    def execute(self, sql, parameters):
        with self.connection.cursor() as cursor:
            cursor.execute(sql, parameters)
            while cursor.nextset():
                pass

    def scalar(self, sql, parameters=()):
        with self.connection.cursor() as cursor:
            cursor.execute(sql, parameters)
            row = cursor.fetchone()
            if row is None:
                self.fail("Expected one SQL row")
            self.assertEqual(len(row), 1)
            self.assertIsNone(cursor.fetchone(), "Expected exactly one SQL row")
            return row[0]

    def captured_prompt(self):
        try:
            path, raw = self.server.requests.get(timeout=2)
        except queue.Empty:
            self.fail("AI_COMPLETE did not reach the recording endpoint")
        self.assertEqual(path, PATH)
        body = json.loads(raw.decode("utf-8"))
        self.assertEqual(body["model"], self.request_model)
        messages = body["messages"]
        self.assertEqual(len(messages), 1)
        self.assertEqual(messages[0]["role"], "user")
        self.assertIsInstance(messages[0]["content"], str)
        self.assertTrue(self.server.requests.empty(), "Unexpected extra HTTP request")
        return messages[0]["content"]

    def complete(self, prompt_expression, parameters):
        self.assertTrue(self.server.requests.empty())
        # Only test-owned SQL expression fragments are composed; ALL values bind.
        result = self.scalar("SELECT AI_COMPLETE(%s, " + prompt_expression + ")",
                             (self.model, *parameters))
        self.assertEqual(result, REPLY)
        return self.captured_prompt()

    def strings(self, template, *arguments):
        expression = "AI_PROMPT(" + ",".join(["%s"] * (1 + len(arguments))) + ")"
        return self.complete(expression, (template, *arguments))

    def assert_deferred(self, expression, parameters, template, arguments):
        envelope = json.loads(self.scalar("SELECT " + expression, parameters))
        self.assertEqual(envelope, {"template": template, "args": arguments})
        self.assertTrue(self.server.requests.empty(), "Constructor sent HTTP")

    def test_longtext_column_completion(self):
        name = "complete_lob_" + uuid.uuid4().hex
        self.execute(f"CREATE TABLE `{name}` (t LONGTEXT)", ())
        try:
            text = "世界" * 40000
            self.execute(f"INSERT INTO `{name}` VALUES (%s)", (text,))
            result = self.scalar(f"SELECT AI_COMPLETE(%s,t) FROM `{name}`", (self.model,))
            self.assertEqual(result, REPLY)
            self.assertEqual(self.captured_prompt(), text)
        finally:
            self.execute(f"DROP TABLE `{name}`", ())

    def test_invalid_json_config_never_sends_http(self):
        for config in ("[]", "null", "1", '"not-an-object"'):
            with self.subTest(config=config):
                with self.assertRaises(pymysql.err.DatabaseError) as caught:
                    self.scalar("SELECT AI_COMPLETE(%s,%s,CAST(%s AS JSON))",
                                (self.model, "bad-config", config))
                self.assertNotIn(caught.exception.args[0], range(2000, 3000))
                self.assertTrue(self.server.requests.empty())
                self.assertEqual(self.scalar("SELECT 1"), 1)

    def test_http_error_is_not_a_valid_completion(self):
        self.server.response_status = 503
        with self.assertRaises(pymysql.err.DatabaseError) as caught:
            self.scalar("SELECT AI_COMPLETE(%s,%s)", (self.model, "http-error"))
        self.assertNotIn(caught.exception.args[0], range(2000, 3000))
        # HTTP 503 is retryable: the native client makes one attempt plus
        # three retries. Validate each real request instead of assuming one.
        self.assertEqual(self.server.requests.qsize(), 4)
        while not self.server.requests.empty():
            path, raw = self.server.requests.get_nowait()
            self.assertEqual(path, PATH)
            body = json.loads(raw)
            self.assertEqual(body["model"], self.request_model)
            self.assertEqual(body["messages"][0]["content"], "http-error")
        self.assertEqual(self.scalar("SELECT 1"), 1)

    def test_null_config_is_absent(self):
        result = self.scalar("SELECT AI_COMPLETE(%s,%s,NULL)", (self.model, "null-config"))
        self.assertEqual(result, REPLY)
        self.assertEqual(self.captured_prompt(), "null-config")

    def test_native_json_config_is_forwarded(self):
        config = {"temperature": 0.2, "max_tokens": 16}
        result = self.scalar("SELECT AI_COMPLETE(%s,%s,CAST(%s AS JSON))",
                             (self.model, "json-config", json.dumps(config)))
        self.assertEqual(result, REPLY)
        path, raw = self.server.requests.get(timeout=2)
        self.assertEqual(path, PATH)
        request = json.loads(raw)
        self.assertEqual(request["temperature"], config["temperature"])
        self.assertEqual(request["max_tokens"], config["max_tokens"])
        self.assertEqual(request["messages"][0]["content"], "json-config")
        self.assertTrue(self.server.requests.empty())

    def test_invalid_completion_responses_are_sql_errors(self):
        for payload in ({"choices": []},
                        {"choices": [{"message": {"content": None}}]},
                        {"choices": [{"message": {"content": ""}}]},
                        {"choices": [{"message": {"content": {"not": "text"}}}]}):
            with self.subTest(payload=payload):
                self.server.response = payload
                with self.assertRaises(pymysql.err.DatabaseError) as caught:
                    self.scalar("SELECT AI_COMPLETE(%s,%s)", (self.model, "invalid-response"))
                self.assertNotIn(caught.exception.args[0], range(2000, 3000))
                self.assertEqual(self.captured_prompt(), "invalid-response")
                self.assertEqual(self.scalar("SELECT 1"), 1)

    def test_repeat_long_string(self):
        argument = 'long-世界-"\\\n' * 4096
        template = "START|" + "|".join(["{0}"] * 9) + "|END"
        self.assertEqual(self.strings(template, argument),
                         "START|" + "|".join([argument] * 9) + "|END")

    def test_array_serialized_only_at_rendering(self):
        value = [1, True, None, {"text": '世界 "\\/\n', "a": [2]}]
        expression = "AI_PROMPT(%s, CAST(%s AS JSON))"
        parameters = ("array={0};end", json.dumps(value, ensure_ascii=False))
        self.assert_deferred(expression, parameters, parameters[0], [value])
        content = self.complete(expression, parameters)
        self.assertTrue(content.startswith("array="))
        self.assertTrue(content.endswith(";end"))
        # Whitespace/key order are not the JSON serialization contract.
        self.assertEqual(json.loads(content[len("array="):-len(";end")]), value)

    def test_object_serialized_only_at_rendering(self):
        value = {"items": [1, False, None], "text": "你好\n{1}"}
        expression = "AI_PROMPT(%s, CAST(%s AS JSON), %s)"
        parameters = ("object={0};tail={1}", json.dumps(value), "tail")
        self.assert_deferred(expression, parameters, parameters[0], [value, "tail"])
        content = self.complete(expression, parameters)
        self.assertTrue(content.startswith("object="))
        self.assertTrue(content.endswith(";tail=tail"))
        self.assertEqual(json.loads(content[len("object="):-len(";tail=tail")]), value)

    def test_multidigit_indices(self):
        arguments = tuple(f"arg-{i}" for i in range(13))
        self.assertEqual(self.strings("{12}|{10}|{1}|{0}", *arguments),
                         "arg-12|arg-10|arg-1|arg-0")

    def test_argument_is_not_recursively_substituted(self):
        self.assertEqual(self.strings("{0}|{1}", "literal {1}", "expanded"),
                         "literal {1}|expanded")

    def test_unicode_quotes_slashes_newlines(self):
        text = '中文 🐬 café "double" \'single\' \\ slash/\nline\r\ttab'
        self.assertEqual(self.strings("模板/\n{0}\n结束", text),
                         "模板/\n" + text + "\n结束")

    def test_malformed_braces_are_literal(self):
        # Existing scanner copies non-placeholder bytes; {{0}} is NOT an escape.
        self.assertEqual(self.strings("{} {-1} {x} { 0 } {0x} } {{0}} {0", "V"),
                         "{} {-1} {x} { 0 } {0x} } {V} {0")

    def test_nested_prompt_is_serialized_envelope_not_recursive_render(self):
        expression = "AI_PROMPT(%s, AI_PROMPT(%s, %s))"
        parameters = ("outer={0};end", "inner {0}", "世界")
        inner = {"template": "inner {0}", "args": ["世界"]}
        self.assert_deferred(expression, parameters, parameters[0], [inner])
        content = self.complete(expression, parameters)
        self.assertTrue(content.startswith("outer="))
        self.assertTrue(content.endswith(";end"))
        self.assertEqual(json.loads(content[len("outer="):-len(";end")]), inner)

    def assert_bad_index(self, template):
        self.assert_deferred("AI_PROMPT(%s, %s)", (template, "only"),
                             template, ["only"])
        try:
            with self.assertRaises(pymysql.err.DatabaseError) as caught:
                self.scalar("SELECT AI_COMPLETE(%s, AI_PROMPT(%s, %s))",
                            (self.model, template, "only"))
            error = caught.exception
            self.assertNotIsInstance(error, pymysql.err.InterfaceError)
            self.assertTrue(error.args and isinstance(error.args[0], int))
            self.assertNotIn(error.args[0], range(2000, 3000),
                             "Client/transport failure is not clean SQL rejection")
            self.assertEqual(self.scalar("SELECT 1"), 1,
                             "Session must survive rejected placeholder")
        finally:
            self.assertTrue(self.server.requests.empty(),
                            "Invalid placeholder must never send HTTP")

    def test_numeric_out_of_range_is_clean_sql_error(self):
        for template in ("{1}", "before {999999} after"):
            with self.subTest(template=template):
                self.assert_bad_index(template)

    def test_overflowing_index_is_clean_sql_error(self):
        # 2**64 may wrap to zero in an unchecked parser and silently use arg[0].
        for digits in ("18446744073709551616", "9" * 200):
            with self.subTest(digits=digits):
                self.assert_bad_index("{" + digits + "}")


if __name__ == "__main__":
    unittest.main()
