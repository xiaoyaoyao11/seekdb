"""Real SQL regressions for native file-as-database (not a query-engine substitute)."""
import json
import os
from pathlib import Path
import tempfile
import unittest

import pymysql


class FileSqlIntegration(unittest.TestCase):
    def connect(self):
        return pymysql.connect(unix_socket=os.environ["SEEKDB_TEST_SOCKET"],
            user="root", database="test", charset="utf8mb4", autocommit=True,
            read_timeout=30, write_timeout=30)

    def setUp(self):
        self.admin = self.connect()
        self.addCleanup(self.admin.close)
        self.tmp = tempfile.TemporaryDirectory(prefix="seekdb-file-sql-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        with self.admin.cursor() as cur:
            cur.execute("SELECT @@global.secure_file_priv")
            row = cur.fetchone()
            assert row is not None
            previous = row[0]
            cur.execute("SET GLOBAL secure_file_priv=%s", (str(self.root),))
        self.addCleanup(self.restore_secure_root, previous)
        self.db = self.connect()
        self.addCleanup(self.db.close)

    def restore_secure_root(self, value):
        with self.admin.cursor() as cur:
            cur.execute("SET GLOBAL secure_file_priv=%s", (value,))

    def sql(self, query, params=()):
        with self.db.cursor() as cur:
            cur.execute(query, params)
            return cur.fetchall()

    def enabled(self):
        self.sql("SET SESSION enable_file_sql=ON")

    def fixture(self, name, data):
        path = self.root / name
        path.write_text(data, encoding="utf8", newline="")
        return str(path)

    def test_describe_select_metadata_not_execution_plan(self):
        rows = self.sql("DESCRIBE SELECT CAST(1 AS SIGNED) AS n")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][:2], ("n", "BIGINT"))
        self.assertEqual(len(rows[0]), 6)

    def test_disabled_by_default(self):
        self.assertEqual(self.sql("SELECT @@session.enable_file_sql"), ((0,),))
        path = self.fixture("disabled.csv", "id\n1\n")
        with self.assertRaises(pymysql.err.DatabaseError) as exc:
            self.sql("SELECT * FROM read_csv(%s)", (path,))
        self.assertEqual(exc.exception.args[0], 1235)
        self.assertEqual(self.sql("SELECT 1+1"), ((2,),))

    def test_csv_projection_aggregate(self):
        self.enabled()
        path = self.fixture("rows.csv", "id,name,score\n1,Alice,3.5\n2,Bob,6.5\n3,Chen,8\n")
        rows = self.sql("SELECT f.name,f.id*2 FROM read_csv(%s) f WHERE id>=2 ORDER BY id DESC LIMIT 1", (path,))
        self.assertEqual(rows, (("Chen", 6),))
        self.assertEqual(self.sql("SELECT COUNT(*),SUM(id),AVG(score) FROM read_csv(%s)", (path,))[0][:2], (3, 6))

    def test_csv_quotes_null_and_duplicate_headers(self):
        self.enabled()
        path = self.fixture("quoted.csv", 'id,note,id,optional\r\n1,"a\n""b""",11,\r\n2,"逗号,ok",12,NULL\r\n')
        self.assertEqual(self.sql("SELECT id,note,id__2,optional FROM read_csv(%s) ORDER BY id", (path,)),
                         ((1, 'a\n"b"', 11, None), (2, "逗号,ok", 12, None)))

    def test_jsonl_union_and_native_cross_format_left_join(self):
        self.enabled()
        csv = self.fixture("left.csv", "id,name\n1,A\n2,B\n3,C\n")
        ndjson = self.fixture("right.jsonl", json.dumps({"id":1,"score":2})+'\n'+json.dumps({"id":2,"score":3.5,"active":True})+'\n')
        self.assertEqual(self.sql("SELECT a.id,a.name,b.score FROM read_csv(%s) a LEFT JOIN read_ndjson(%s) b ON a.id=b.id ORDER BY a.id", (csv,ndjson)),
                         ((1,"A",2.0),(2,"B",3.5),(3,"C",None)))
        self.assertEqual(self.sql("SELECT id,active FROM read_json(%s,format='newline_delimited') ORDER BY id", (ndjson,)),
                         ((1,None),(2,1)))

    def test_parquet_projection_aggregate_and_csv_join(self):
        import base64
        self.enabled()
        public = Path(__file__).resolve().parents[2]/"tools/deploy/mysql_test/test_suite/file_sql/data/simple.parquet.b64"
        path = self.root/"values.parquet"
        path.write_bytes(base64.b64decode(public.read_bytes()))
        self.assertEqual(self.sql("SELECT COUNT(*),MIN(c1),MAX(c1),SUM(c1) FROM read_parquet(%s)", (str(path),)),
                         ((7,0,6,21),))
        csv = self.fixture("names.csv", "id,name\n0,zero\n6,six\n")
        self.assertEqual(self.sql("SELECT p.c1,n.name FROM read_parquet(%s) p INNER JOIN read_csv(%s) n ON p.c1=n.id ORDER BY p.c1", (str(path),csv)),
                         ((0,"zero"),(6,"six")))

    def test_glob_returns_only_file_column(self):
        self.enabled()
        one = self.fixture("one.csv", "id\n1\n")
        two = self.fixture("two.csv", "id\n2\n")
        self.fixture("ignored.jsonl", '{"id":3}\n')
        self.assertEqual(self.sql("SELECT file FROM glob(%s) ORDER BY file", (str(self.root/"*.csv"),)),
                         ((one,),(two,)))

    def test_describe_file_types_and_group_having(self):
        self.enabled()
        path = self.fixture("types.csv", "id,score,active,day,created,missing\n1,1.5,true,2024-02-29,2024-02-29T12:00:00,NULL\n2,2,false,,,NULL\n")
        rows = self.sql("DESCRIBE SELECT * FROM read_csv(%s)", (path,))
        self.assertEqual([r[:3] for r in rows], [("id","BIGINT","NO"),("score","DOUBLE","NO"),
            ("active","BOOLEAN","NO"),("day","DATE","YES"),("created","DATETIME","YES"),("missing","NULL","YES")])
        self.assertEqual(self.sql("SELECT active,COUNT(*),SUM(id) FROM read_csv(%s) GROUP BY active HAVING SUM(id)>1 ORDER BY active", (path,)),
                         ((0,1,2),))

    def test_copy_roundtrip_and_no_overwrite(self):
        self.enabled()
        source=self.fixture("source.csv", 'id,name\n1,"世界,hi"\n2,Bob\n')
        output=str(self.root/"export.csv")
        self.sql("COPY (SELECT id,name FROM read_csv(%s) ORDER BY id) TO %s (FORMAT CSV, HEADER, NULLSTR 'NULL')",(source,output))
        self.assertEqual(self.sql("SELECT id,name FROM read_csv(%s) ORDER BY id",(output,)),((1,"世界,hi"),(2,"Bob")))
        before=Path(output).read_bytes()
        with self.assertRaises(pymysql.err.DatabaseError):
            self.sql("COPY (SELECT 99 AS id) TO %s (HEADER)",(output,))
        self.assertEqual(Path(output).read_bytes(),before)

    def test_copy_preserves_empty_literal_null_and_sql_null(self):
        self.enabled()
        output=str(self.root/"nulls.csv")
        self.sql("COPY (SELECT '' AS blank, 'NULL' AS literal_null, CAST(NULL AS CHAR) AS missing) TO %s (HEADER)",(output,))
        self.assertEqual(self.sql("SELECT blank,literal_null,missing FROM read_csv(%s)",(output,)),(("","NULL",None),))

    def test_copy_table_custom_dialect_and_security(self):
        import uuid
        self.enabled()
        name="file_copy_"+uuid.uuid4().hex
        self.sql(f"CREATE TABLE `{name}` (id INT, name VARCHAR(50))")
        try:
            self.sql(f"INSERT INTO `{name}` VALUES (1,'a|b'),(2,'c!d')")
            output=str(self.root/"custom.csv")
            self.sql(f"COPY `{name}` TO %s (HEADER, DELIMITER '|', QUOTE '!', ESCAPE '!')",(output,))
            self.assertEqual(self.sql("SELECT id,name FROM read_csv(%s,delim='|',quote='!',escape='!') ORDER BY id",(output,)),((1,"a|b"),(2,"c!d")))
            with tempfile.TemporaryDirectory(prefix="file-sql-copy-outside-") as outside:
                target=Path(outside)/"escape.csv"
                with self.assertRaises(pymysql.err.DatabaseError):
                    self.sql("COPY (SELECT 1 AS n) TO %s (HEADER)",(str(target),))
                self.assertFalse(target.exists())
        finally:self.sql(f"DROP TABLE `{name}`")

    def test_copy_failure_cleans_temporary_file(self):
        self.enabled()
        source=self.fixture("input.csv","id\n1\n2\n")
        target=self.root/"failed.csv"
        with self.assertRaises(pymysql.err.DatabaseError):
            self.sql("COPY (SELECT JSON_EXTRACT(IF(id=1,'{\"a\":1}','broken'),'$.a') AS a FROM read_csv(%s)) TO %s (HEADER)",(source,str(target)))
        self.assertFalse(target.exists())
        self.assertEqual(list(self.root.glob('.seekdb-copy-*')),[])
        self.assertEqual(self.sql("SELECT 1"),((1,),))

    def test_copy_rejects_invalid_dialect_without_files(self):
        self.enabled()
        for i,options in enumerate(("DELIMITER '\\n'", "QUOTE '\\r'", "NULLSTR 'x,y'")):
            target=self.root/("invalid-"+str(i)+".csv")
            with self.subTest(options=options),self.assertRaises(pymysql.err.DatabaseError):
                self.sql("COPY (SELECT 1 AS n) TO %s ("+options+")",(str(target),))
            self.assertFalse(target.exists())
        self.assertEqual(list(self.root.glob('.seekdb-copy-*')),[])

    def test_schema_is_reinferred_and_datetime_precision_preserved(self):
        import datetime
        self.enabled()
        path=self.fixture("changing.csv","id,created\n1,2024-02-29T12:00:00.123456\n")
        self.assertEqual(self.sql("SELECT * FROM read_csv(%s)",(path,)),((1,datetime.datetime(2024,2,29,12,0,0,123456)),))
        Path(path).write_text("id,name,flag\n2,changed,true\n")
        self.assertEqual(self.sql("SELECT * FROM read_csv(%s)",(path,)),((2,"changed",1),))
        plan=self.sql("EXPLAIN SELECT 1")
        self.assertGreater(len(plan),1)

    def test_native_nested_loop_rescans(self):
        self.enabled()
        left=self.fixture("a.csv","id\n1\n2\n3\n")
        right=self.fixture("b.csv","id,value\n1,a\n2,b\n3,c\n")
        self.assertEqual(self.sql("SELECT /*+ USE_NL(b) */ a.id,b.value FROM read_csv(%s) a JOIN read_csv(%s) b ON a.id=b.id ORDER BY a.id",(left,right)),((1,"a"),(2,"b"),(3,"c")))

    def test_signed_numeric_inference_and_execution_match(self):
        self.enabled()
        path=self.fixture("plus.csv","i,d,e\n+1,+1.5,+1e2\n+2,-2.5,+2e2\n")
        self.assertEqual(self.sql("SELECT i,d,e FROM read_csv(%s) ORDER BY i",(path,)),((1,1.5,100.0),(2,-2.5,200.0)))
        self.assertEqual(self.sql("SELECT SUM(i),SUM(d),SUM(e) FROM read_csv(%s)",(path,)),((3,-1.0,300.0),))

    def test_file_privilege_required_before_inference(self):
        import uuid
        name="filepriv_"+uuid.uuid4().hex
        self.sql(f"CREATE USER `{name}`@'%%'")
        self.addCleanup(self.sql,f"DROP USER `{name}`@'%%'")
        self.sql(f"GRANT SELECT ON test.* TO `{name}`@'%%'")
        csv=self.fixture("private.csv","id\n1\n")
        jsonl=self.fixture("private.jsonl",'{"id":1}\n')
        import base64
        public=Path(__file__).resolve().parents[2]/"tools/deploy/mysql_test/test_suite/file_sql/data/simple.parquet.b64"
        parquet=self.root/"private.parquet"
        parquet.write_bytes(base64.b64decode(public.read_bytes()))
        def connect_user():
            return pymysql.connect(unix_socket=os.environ["SEEKDB_TEST_SOCKET"],user=name,database="test",autocommit=True,charset="utf8mb4")
        with connect_user() as connection:
            with connection.cursor() as cur:
                cur.execute("SET SESSION enable_file_sql=ON")
                for fn,path in (("read_csv",csv),("read_json",jsonl),("read_ndjson",jsonl),("read_parquet",str(parquet)),("glob",str(self.root/"*")),("read_csv",str(self.root/"nonexistent.csv"))):
                    with self.subTest(function=fn,path=path):
                        with self.assertRaises(pymysql.err.DatabaseError) as denied:
                            cur.execute("SELECT * FROM "+fn+"(%s)",(path,))
                        self.assertIn("privilege",denied.exception.args[1].lower())
                for query,path in (("DESCRIBE SELECT * FROM read_csv(%s)",csv),
                                   ("EXPLAIN SELECT * FROM read_csv(%s)",csv),
                                   ("COPY (SELECT 1 AS id) TO %s (HEADER TRUE)",str(self.root/"denied.csv"))):
                    with self.subTest(query=query):
                        with self.assertRaises(pymysql.err.DatabaseError) as denied:
                            cur.execute(query,(path,))
                        self.assertIn("privilege",denied.exception.args[1].lower())
                self.assertFalse((self.root/"denied.csv").exists())
        self.sql(f"GRANT FILE ON *.* TO `{name}`@'%%'")
        with connect_user() as connection:
            with connection.cursor() as cur:
                cur.execute("SET SESSION enable_file_sql=ON")
                cur.execute("SELECT id FROM read_csv(%s)",(csv,))
                self.assertEqual(cur.fetchall(),((1,),))
        self.sql(f"REVOKE FILE ON *.* FROM `{name}`@'%%'")
        with connect_user() as connection:
            with connection.cursor() as cur:
                cur.execute("SET SESSION enable_file_sql=ON")
                with self.assertRaises(pymysql.err.DatabaseError) as denied:
                    cur.execute("DESCRIBE SELECT * FROM read_csv(%s)",(csv,))
                self.assertIn("privilege",denied.exception.args[1].lower())

    def test_unsupported_sampling_is_rejected(self):
        self.enabled()
        path = self.fixture("sample.csv", "id\n1\n")
        with self.assertRaises(pymysql.err.DatabaseError) as exc:
            self.sql("SELECT * FROM read_csv(%s,sample_size=10)", (path,))
        self.assertEqual(exc.exception.args[0], 1235)

    def test_secure_root_cannot_be_escaped(self):
        self.enabled()
        with tempfile.TemporaryDirectory(prefix="outside-file-sql-") as outside:
            path = Path(outside)/"secret.csv"
            path.write_text("secret\n99\n")
            link = self.root/"escape.csv"
            link.symlink_to(path)
            for target in (str(path), str(link), str(self.root/".."/Path(outside).name/"secret.csv")):
                with self.subTest(target=target), self.assertRaises(pymysql.err.DatabaseError):
                    self.sql("SELECT * FROM read_csv(%s)", (target,))
        self.assertEqual(self.sql("SELECT 1"), ((1,),))

    def test_json_nested_and_bad_csv_fail_cleanly(self):
        self.enabled()
        nested = self.fixture("nested.jsonl", '{"a":{"nested":1}}\n')
        broken = self.fixture("broken.csv", 'id,name\n1,"unterminated\n')
        for fn,path in (("read_json",nested),("read_csv",broken)):
            with self.subTest(fn=fn), self.assertRaises(pymysql.err.DatabaseError):
                self.sql("SELECT * FROM "+fn+"(%s)",(path,))
        self.assertEqual(self.sql("SELECT 1"), ((1,),))


if __name__ == "__main__":
    unittest.main()
