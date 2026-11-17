"""Native seekdb SQL regression tests; requires PyMySQL and a running server.

From the repository root, FIRST run against the UNMODIFIED baseline:
    python tests/competition/test_ai_prompt_integration.py --first
Equivalent single-test invocation:
    python tests/competition/test_ai_prompt_integration.py \
        AIPromptIntegration.test_json_array_roundtrip -v
Only that test runs: a native JSON array must remain an array inside args[0].
A SQL rejection or structural assertion failure is RED; a connection/import
failure is an environment blocker, not evidence of the regression. Preserve
that baseline result before editing production code. Then run the whole file:
    python tests/competition/test_ai_prompt_integration.py -v

Configuration (all optional): SEEKDB_TEST_HOST=127.0.0.1,
SEEKDB_TEST_PORT=2886, SEEKDB_TEST_USER=root, SEEKDB_TEST_DATABASE=test,
SEEKDB_TEST_PASSWORD (unset means empty), SEEKDB_TEST_SOCKET (optional Unix socket).
A socket allows the isolated test server to disable TCP listening entirely.
Never print connection credentials.
The existing database must be dedicated to local tests; no database is created.
Tables have UUID names and are dropped only after this test creates them.

AI_PROMPT returns an envelope, NOT rendered text. These tests deliberately
require no model registration, network endpoint, API key, or AI_COMPLETE call.
Repeated/malformed/out-of-range placeholders below test deferred construction
only, not substitution. Rendering acceptance needs separate AI_COMPLETE tests
with a recording OpenAI-compatible endpoint and assertions on captured content.
"""

import json
import os
import sys
import unittest
import uuid

import pymysql


class AIPromptIntegration(unittest.TestCase):
    def setUp(self):
        self.connection = pymysql.connect(
            host=os.environ.get("SEEKDB_TEST_HOST", "127.0.0.1"),
            unix_socket=os.environ.get("SEEKDB_TEST_SOCKET") or None,
            port=int(os.environ.get("SEEKDB_TEST_PORT", "2886")),
            user=os.environ.get("SEEKDB_TEST_USER", "root"),
            password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
            database=os.environ.get("SEEKDB_TEST_DATABASE", "test"),
            charset="utf8mb4",
            autocommit=True,
            connect_timeout=10,
            read_timeout=30,
            write_timeout=30,
        )
        self.addCleanup(self.connection.close)

    def scalar(self, sql, parameters=()):
        with self.connection.cursor() as cursor:
            cursor.execute(sql, parameters)
            row = cursor.fetchone()
            if row is None:
                self.fail("SQL must return one row")
            self.assertEqual(len(row), 1, "SQL must return one column")
            self.assertIsNone(cursor.fetchone(), "SQL must return exactly one row")
            return row[0]

    def envelope(self, sql, parameters=()):
        value = self.scalar(sql, parameters)
        self.assertIsInstance(value, (str, bytes), "Expected serialized JSON")
        result = json.loads(value)
        self.assertIsInstance(result, dict, "AI_PROMPT must return an object")
        self.assertEqual(set(result), {"template", "args"})
        self.assertIsInstance(result["template"], str)
        self.assertIsInstance(result["args"], list)
        return result

    def assert_prompt(self, sql, template, args, parameters=()):
        self.assertEqual(
            self.envelope(sql, parameters), {"template": template, "args": args}
        )

    def table(self, columns):
        name = "ai_prompt_reg_" + uuid.uuid4().hex
        with self.connection.cursor() as cursor:
            # No IF NOT EXISTS: a collision must not adopt somebody else's table.
            cursor.execute(f"CREATE TABLE `{name}` ({columns})")
        self.addCleanup(self.drop_table, name)
        return f"`{name}`"

    def drop_table(self, name):
        with self.connection.cursor() as cursor:
            cursor.execute(f"DROP TABLE `{name}`")

    def assert_sql_error(self, sql, parameters=(), codes=None):
        # Do not mistake a disconnect/server crash/client timeout for SQL rejection.
        with self.assertRaises(pymysql.err.DatabaseError) as caught:
            self.scalar(sql, parameters)
        error = caught.exception
        self.assertNotIsInstance(error, pymysql.err.InterfaceError)
        self.assertTrue(error.args and isinstance(error.args[0], int))
        self.assertNotIn(error.args[0], range(2000, 3000),
                         "Client/transport failure is not a valid SQL error")
        if codes is not None:
            self.assertIn(error.args[0], codes)
        self.assertEqual(self.scalar("SELECT 1"), 1,
                         "Session must remain usable after rejected input")

    def test_official_array_object_example(self):
        self.assert_prompt(
            "SELECT AI_PROMPT('{0}/{0}/{1}', JSON_ARRAY(1,true), JSON_OBJECT('k',2))",
            "{0}/{0}/{1}", [[1, True], {"k": 2}],
        )

    def test_json_array_roundtrip(self):
        result = self.envelope("SELECT AI_PROMPT('{0}', JSON_ARRAY(1,true))")
        self.assertEqual(result["template"], "{0}")
        self.assertEqual(len(result["args"]), 1)
        self.assertIsInstance(result["args"][0], list,
                              "JSON_ARRAY must not be serialized as a quoted string")
        self.assertEqual(result["args"][0], [1, True])
        self.assertIs(type(result["args"][0][1]), bool)

    def test_json_object_and_nested_arrays_preserve_structure(self):
        expected = {"k": [1, True, None, {"inner": "世界"}]}
        self.assert_prompt(
            "SELECT AI_PROMPT('{0}', "
            "JSON_OBJECT('k', JSON_ARRAY(1,true,NULL,JSON_OBJECT('inner',%s))))",
            "{0}", [expected], ("世界",),
        )

    def test_nested_prompt_remains_structured_and_deferred(self):
        inner = {"template": "inner {0}", "args": ["世界"]}
        self.assert_prompt(
            "SELECT AI_PROMPT('outer {0}', AI_PROMPT('inner {0}', %s))",
            "outer {0}", [inner], ("世界",),
        )

    def test_mixed_string_object_array_arguments(self):
        self.assert_prompt(
            "SELECT AI_PROMPT('{0}/{1}/{2}', %s, "
            "JSON_OBJECT('k',2), JSON_ARRAY('x',JSON_ARRAY(3)))",
            "{0}/{1}/{2}", ["plain", {"k": 2}, ["x", [3]]], ("plain",),
        )

    def test_unicode_and_escaping_are_lossless(self):
        text = '中文 🐬 café "quoted" \\ slash/\nline\r\ttab\x00end {0}'
        self.assert_prompt("SELECT AI_PROMPT(%s, %s)", text, [text], (text, text))
        self.assert_prompt(
            "SELECT AI_PROMPT('{0}', JSON_OBJECT('value', %s), JSON_ARRAY(%s))",
            "{0}", [{"value": text}, [text]], (text, text),
        )

    def test_text_and_longtext_columns_in_both_positions(self):
        table = self.table("id INT PRIMARY KEY, t TEXT, a TEXT, lt LONGTEXT, la LONGTEXT")
        template = "文本 {0} " + "界" * 19000
        argument = "参数\n" + "海" * 19000
        long_template = "长模板 {0} " + "洋" * 40000
        long_argument = '长参数 "\\\n' + "涛" * 40000
        self.assertLessEqual(len(template.encode("utf-8")), 65535)
        self.assertLessEqual(len(argument.encode("utf-8")), 65535)
        self.assertGreater(len(long_template.encode("utf-8")), 65536)
        self.assertGreater(len(long_argument.encode("utf-8")), 65536)
        with self.connection.cursor() as cursor:
            cursor.execute(f"INSERT INTO {table} VALUES (1,%s,%s,%s,%s)",
                           (template, argument, long_template, long_argument))
        for template_column, arg_column, expected_template, expected_arg in (
            ("t", "a", template, argument),
            ("lt", "la", long_template, long_argument),
            ("t", "la", template, long_argument),
            ("lt", "a", long_template, argument),
        ):
            with self.subTest(template=template_column, argument=arg_column):
                self.assert_prompt(
                    f"SELECT AI_PROMPT({template_column},{arg_column}) FROM {table}",
                    expected_template, [expected_arg],
                )

    def test_placeholder_validation_is_deferred(self):
        for template, args in (
            ("{0}/{0}/{0}", ["value"]),
            ("{1} {999999}", ["unused"]),
            ("{0}", []),
            ("plain template", ["unused"]),
            ("{} {-1} {x} { 0 } {{0}} {0", ["literal"]),
        ):
            with self.subTest(template=template):
                sql = "SELECT AI_PROMPT(" + ",".join(["%s"] * (1 + len(args))) + ")"
                self.assert_prompt(sql, template, args, (template, *args))

    def test_deferred_envelope_can_be_stored_without_rendering(self):
        table = self.table("id INT PRIMARY KEY, p JSON")
        template = "unused {999999}; literal {bad}; repeated {0}/{0}"
        with self.connection.cursor() as cursor:
            cursor.execute(
                f"INSERT INTO {table} VALUES (1, AI_PROMPT(%s, JSON_ARRAY(1,true)))",
                (template,),
            )
        self.assertEqual(self.envelope(f"SELECT p FROM {table} WHERE id=1"),
                         {"template": template, "args": [[1, True]]})

    def test_empty_string_argument_and_no_arguments(self):
        self.assert_prompt("SELECT AI_PROMPT('plain')", "plain", [])
        self.assert_prompt("SELECT AI_PROMPT('{0}', '')", "{0}", [""])

    def test_invalid_template_types_are_sql_errors(self):
        for expression in ("1", "1.25", "true", "JSON_OBJECT('k',1)", "JSON_ARRAY(1)"):
            with self.subTest(expression=expression):
                self.assert_sql_error(f"SELECT AI_PROMPT({expression})", codes=(5083,))

    def test_invalid_argument_types_are_sql_errors(self):
        for expression in ("1", "1.25", "true", "CAST('2026-01-01' AS DATE)"):
            with self.subTest(expression=expression):
                self.assert_sql_error(f"SELECT AI_PROMPT('{{0}}',{expression})", codes=(5083,))

    def test_literal_sql_null_is_rejected(self):
        self.assert_sql_error("SELECT AI_PROMPT(NULL)", codes=(5083,))
        self.assert_sql_error("SELECT AI_PROMPT('{0}',NULL)", codes=(5083,))

    def test_nullable_columns_are_sql_errors_not_crashes(self):
        table = self.table("t TEXT NULL, j JSON NULL")
        with self.connection.cursor() as cursor:
            cursor.execute(f"INSERT INTO {table} VALUES (NULL,NULL)")
        for expression in ("AI_PROMPT(t)", "AI_PROMPT('{0}',t)", "AI_PROMPT('{0}',j)"):
            with self.subTest(expression=expression):
                # Runtime NULL in a typed column need not share literal NULL's code.
                self.assert_sql_error(f"SELECT {expression} FROM {table}")

    def test_empty_template_is_rejected(self):
        # OB_INVALID_ARGUMENT is 4002 internally, mapped to MySQL 1210 in some builds.
        self.assert_sql_error("SELECT AI_PROMPT('')", codes=(4002, 1210))


if __name__ == "__main__":
    if "--first" in sys.argv:
        if sys.argv[1:] != ["--first"]:
            raise SystemExit("Use --first alone to run ONLY the JSON_ARRAY roundtrip")
        unittest.main(argv=[sys.argv[0],
                            "AIPromptIntegration.test_json_array_roundtrip", "-v"])
    else:
        unittest.main()
