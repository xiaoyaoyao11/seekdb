#!/usr/bin/env python3
"""Extra local checks for requirements the public samples do not cover.

Covers: illegal vectors (NaN/Inf/overflow/dimension), Unicode VARCHAR length,
code-point ordering, SELECT *, explicit column list, comparison operators,
ORDER BY ties, LIMIT 0, hard-kill crash recovery, torn WAL tail, repeated
recovery, and a 10k/128-dim scale + TopK timing smoke test.

It also covers the SQL that the public samples never use but the official
case set may: NULL values, IS [NOT] NULL, OR / NOT, LIKE, BETWEEN, IN,
LIMIT with OFFSET, SELECT DISTINCT, implicit aliases and expressions on the
right hand side of an UPDATE.
"""
import argparse
import json
import math
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from local_check import Session  # noqa: E402


def q(sess, sql, mode="exact"):
    return sess.request(sql, mode)


def expect_error(sess, sql, label, out):
    r = q(sess, sql)
    assert r.get("ok") is False, f"{label}: expected error, got {r!r}"
    assert isinstance(r.get("error", {}).get("code"), str) and r["error"]["code"], f"{label}: bad error {r!r}"
    out.append((label, "PASS", r["error"]["code"]))


def expect_rows(sess, sql, rows, label, out, mode="exact"):
    r = q(sess, sql, mode)
    assert r.get("ok") is True, f"{label}: {r!r}"
    assert r.get("rows") == rows, f"{label}: rows {r.get('rows')!r} != {rows!r}"
    out.append((label, "PASS", ""))


def boundary_tests(sess, out):
    q(sess, "CREATE TABLE t (id BIGINT PRIMARY KEY, tag VARCHAR(4), txt VARCHAR(64), e VECTOR(3));")
    expect_error(sess, "INSERT INTO t VALUES (1,'x','NaN','[NaN,0,0]');", "reject NaN", out)
    expect_error(sess, "INSERT INTO t VALUES (1,'x','Inf','[Infinity,0,0]');", "reject Infinity", out)
    expect_error(sess, "INSERT INTO t VALUES (1,'x','dim','[0,0]');", "reject dimension mismatch", out)
    expect_error(sess, "INSERT INTO t VALUES (1,'x','over','[1e300,0,0]');", "reject float32 overflow", out)
    expect_error(sess, "INSERT INTO t VALUES (1,'x','bad','[a,0,0]');", "reject non-numeric vector", out)
    expect_error(sess, "INSERT INTO t VALUES (1,'中文中文中文','too long','[0,0,0]');", "Unicode VARCHAR length", out)
    expect_error(sess, "INSERT INTO t VALUES (1,'x','dup','[0,0,0]'),(1,'y','dup2','[1,0,0]');",
                 "statement atomicity on duplicate key", out)
    r = q(sess, "SELECT id FROM t;")
    assert r["rows"] == [], f"failed INSERT left rows behind: {r!r}"
    out.append(("no partial row after failed INSERT", "PASS", ""))

    q(sess, "INSERT INTO t VALUES (1,'甲','中文测试','[1,0,0]'),(2,'乙','乙文','[0,1,0]'),(3,'甲','甲文','[0,0,1]');")
    expect_rows(sess, "SELECT * FROM t ORDER BY id;",
                [[1, "甲", "中文测试", [1.0, 0.0, 0.0]],
                 [2, "乙", "乙文", [0.0, 1.0, 0.0]],
                 [3, "甲", "甲文", [0.0, 0.0, 1.0]]],
                "SELECT * with vector projection", out)
    expect_rows(sess, "SELECT id, e FROM t WHERE id = 2;", [[2, [0.0, 1.0, 0.0]]], "vector column projection", out)
    # U+4E59 (乙) < U+7532 (甲); UTF-8 byte order preserves code-point order.
    expect_rows(sess, "SELECT id FROM t WHERE tag < '甲' ORDER BY id;", [[2]],
                "Unicode code-point comparison", out)
    expect_rows(sess, "SELECT id FROM t WHERE id <= 2 AND id > 0 ORDER BY id;", [[1], [2]],
                "AND with <= and >", out)
    expect_rows(sess, "SELECT id FROM t WHERE tag = '甲' ORDER BY id;", [[1], [3]],
                "ORDER BY tie-break on primary key", out)
    # 甲 (U+7532) sorts after 乙 (U+4E59); DESC puts 甲 first, PK breaks ties.
    expect_rows(sess, "SELECT id FROM t ORDER BY tag DESC;", [[1], [3], [2]],
                "DESC ordering with PK tie-break", out)
    expect_rows(sess, "SELECT id FROM t ORDER BY id LIMIT 0;", [], "LIMIT 0", out)
    expect_rows(sess, "SELECT id, L2_DISTANCE(e, '[0,0,0]') AS d FROM t WHERE tag = '甲' ORDER BY d LIMIT 5;",
                [[1, 1.0], [3, 1.0]], "filtered TopK with distance ties", out)
    expect_rows(sess, "SELECT id, L2_DISTANCE(e, '[1,0,0]') AS d FROM t ORDER BY d LIMIT 10;",
                [[1, 0.0], [2, math.sqrt(2.0)], [3, math.sqrt(2.0)]],
                "TopK returns all when fewer than K", out)
    expect_rows(sess, "SELECT id, L2_DISTANCE(e, '[0,0,0]') AS d FROM t WHERE tag = 'absent' ORDER BY d;",
                [], "empty filtered TopK", out)
    expect_rows(sess, "SELECT id, L2_DISTANCE(e, '[1,0,0]') AS d FROM t ORDER BY d LIMIT 2;",
                [[1, 0.0], [2, math.sqrt(2.0)]], "ann mode exact-quality", out, mode="ann")

    # explicit column list, UPDATE / DELETE by condition, transaction visibility
    q(sess, "INSERT INTO t (id, tag, txt, e) VALUES (4,'丙','四','[1,1,0]'),(5,'丙','五','[1,0,1]');")
    expect_rows(sess, "SELECT id FROM t WHERE tag = '丙' ORDER BY id;", [[4], [5]], "explicit column list", out)
    q(sess, "BEGIN;")
    q(sess, "UPDATE t SET txt = '改' WHERE tag = '丙';")
    expect_rows(sess, "SELECT id, txt FROM t WHERE tag = '丙' ORDER BY id;", [[4, "改"], [5, "改"]],
                "read own writes", out)
    r = q(sess, "SELECT id, L2_DISTANCE(e, '[1,1,0]') AS d FROM t ORDER BY d LIMIT 1;")
    assert r["rows"][0][0] == 4, f"vector search in txn stale: {r!r}"
    out.append(("vector search sees txn state", "PASS", ""))
    q(sess, "ROLLBACK;")
    expect_rows(sess, "SELECT id, txt FROM t WHERE tag = '丙' ORDER BY id;", [[4, "四"], [5, "五"]],
                "rollback restores rows", out)
    expect_error(sess, "ALTER TABLE t ADD x BIGINT;", "unsupported ALTER", out)
    expect_error(sess, "DROP TABLE t;", "unsupported DROP", out)
    expect_error(sess, "SELECT COUNT(*) FROM t;", "unsupported aggregate", out)
    q(sess, "DELETE FROM t WHERE tag = '丙';")
    expect_rows(sess, "SELECT id FROM t ORDER BY id;", [[1], [2], [3]], "conditional DELETE", out)


def sql_feature_tests(sess, out):
    """The SQL that the public samples do not use but the official set may."""
    q(sess, "CREATE TABLE n (id BIGINT PRIMARY KEY, v BIGINT, c VARCHAR(20), e VECTOR(2));")
    q(sess, "INSERT INTO n VALUES (1, 10, 'alpha', '[1,0]'), (2, 20, 'beta', '[0,1]'), "
             "(3, NULL, NULL, '[2,2]');")

    # NULL is a value of every type and it survives a restart.
    expect_rows(sess, "SELECT id, v, c, e FROM n WHERE id = 3;",
                [[3, None, None, [2.0, 2.0]]], "NULL is a value", out)
    expect_rows(sess, "SELECT id FROM n WHERE v IS NULL;", [[3]], "IS NULL", out)
    expect_rows(sess, "SELECT id FROM n WHERE v IS NOT NULL ORDER BY id;", [[1], [2]],
                "IS NOT NULL", out)
    expect_rows(sess, "SELECT id FROM n WHERE v = NULL;", [], "a comparison with NULL is unknown", out)
    expect_rows(sess, "SELECT id FROM n WHERE v != NULL;", [], "an inequality with NULL is unknown", out)
    expect_rows(sess, "SELECT id FROM n ORDER BY v;", [[3], [1], [2]],
                "NULLs sort first ascending", out)
    expect_rows(sess, "SELECT id FROM n ORDER BY v DESC;", [[2], [1], [3]],
                "NULLs sort last descending", out)

    # The boolean operators.
    expect_rows(sess, "SELECT id FROM n WHERE id = 1 OR id = 2 ORDER BY id;", [[1], [2]],
                "OR", out)
    expect_rows(sess, "SELECT id FROM n WHERE NOT (id = 1) ORDER BY id;", [[2], [3]],
                "NOT", out)
    expect_rows(sess, "SELECT id FROM n WHERE (v = 10 OR v = 20) AND id > 1 ORDER BY id;", [[2]],
                "parenthesised OR with AND", out)

    # The other comparison forms.
    expect_rows(sess, "SELECT id FROM n WHERE c LIKE 'al%' ORDER BY id;", [[1]], "LIKE", out)
    expect_rows(sess, "SELECT id FROM n WHERE c LIKE '%eta' ORDER BY id;", [[2]], "LIKE with a suffix", out)
    expect_rows(sess, "SELECT id FROM n WHERE id BETWEEN 1 AND 2 ORDER BY id;", [[1], [2]],
                "BETWEEN", out)
    expect_rows(sess, "SELECT id FROM n WHERE id IN (1,3) ORDER BY id;", [[1], [3]], "IN", out)

    # The paging forms.
    expect_rows(sess, "SELECT id FROM n ORDER BY id LIMIT 1 OFFSET 1;", [[2]], "LIMIT with OFFSET", out)
    expect_rows(sess, "SELECT id FROM n ORDER BY id LIMIT 5 OFFSET 10;", [],
                "OFFSET past the end", out)

    # The projection forms.
    expect_rows(sess, "SELECT DISTINCT v FROM n WHERE v IS NOT NULL ORDER BY v;", [[10], [20]],
                "SELECT DISTINCT", out)
    r = q(sess, "SELECT c label FROM n WHERE id = 1;")
    assert r.get("columns") == ["label"], f"implicit alias: {r!r}"
    out.append(("implicit alias", "PASS", ""))

    # An expression on the right hand side of an assignment.
    q(sess, "CREATE TABLE m (id BIGINT PRIMARY KEY, n BIGINT);")
    q(sess, "INSERT INTO m VALUES (1, 5);")
    q(sess, "UPDATE m SET n = n + 1 WHERE id = 1;")
    expect_rows(sess, "SELECT n FROM m;", [[6]], "UPDATE with an expression", out)
    q(sess, "UPDATE m SET n = n * 2 WHERE id = 1;")
    expect_rows(sess, "SELECT n FROM m;", [[12]], "UPDATE with a multiplication", out)

    # A null value cannot be the primary key.
    expect_error(sess, "INSERT INTO m (id, n) VALUES (NULL, 1);", "NULL primary key", out)


def crash_tests(exe, out_dir, out):
    d = Path(tempfile.mkdtemp(prefix="mvdb-crash-"))
    log = out_dir / "crash.stderr.log"
    s = Session(str(exe), str(d), log)
    q(s, "CREATE TABLE docs (id BIGINT PRIMARY KEY, v VARCHAR(32), e VECTOR(4));")
    q(s, "INSERT INTO docs VALUES (1,'a','[1,0,0,0]'),(2,'b','[0,1,0,0]');")
    s.shutdown()
    s.spawn()
    expect_rows(s, "SELECT id FROM docs ORDER BY id;", [[1], [2]], "committed rows survive restart", out)

    q(s, "BEGIN;")
    q(s, "INSERT INTO docs VALUES (3,'c','[0,0,1,0]');")
    q(s, "UPDATE docs SET v = 'changed' WHERE id = 1;")
    expect_rows(s, "SELECT v FROM docs WHERE id = 1;", [["changed"]], "uncommitted write visible in txn", out)
    s.crash()          # hard kill, no shutdown response
    s.spawn()
    expect_rows(s, "SELECT id, v FROM docs ORDER BY id;", [[1, "a"], [2, "b"]],
                "hard kill rolls back uncommitted txn", out)

    q(s, "INSERT INTO docs VALUES (4,'d','[0,0,0,1]');")
    s.shutdown()
    # Append a torn WAL record (no newline, bad CRC) to mimic a killed writer.
    wal = d / "wal.log"
    with open(wal, "ab") as f:
        f.write(b"12345\t{\"ops\":[[\"put\"")
    s.spawn()
    expect_rows(s, "SELECT id FROM docs ORDER BY id;", [[1], [2], [4]],
                "torn WAL tail is discarded", out)
    # Kill again immediately during startup-ish state, then recover once more.
    s.shutdown()
    s.spawn()
    s.crash()
    s.spawn()
    expect_rows(s, "SELECT id FROM docs ORDER BY id;", [[1], [2], [4]],
                "repeated recovery is idempotent", out)
    s.shutdown()
    s.close()


def scale_test(exe, out_dir, out, n=10000, dim=128, queries=200):
    d = Path(tempfile.mkdtemp(prefix="mvdb-scale-"))
    log = out_dir / "scale.stderr.log"
    s = Session(str(exe), str(d), log)
    q(s, f"CREATE TABLE bench (id BIGINT PRIMARY KEY, cat VARCHAR(8), e VECTOR({dim}));")
    batch = 100
    t0 = time.perf_counter()
    for start in range(0, n, batch):
        vals = []
        for i in range(start, min(start + batch, n)):
            vec = [round(((i * (k + 1)) % 97) / 97.0, 4) for k in range(dim)]
            vals.append(f"({i},'c{i % 10}','[{','.join(str(x) for x in vec)}]')")
        r = q(s, "INSERT INTO bench VALUES " + ",".join(vals) + ";")
        assert r.get("ok") is True, f"bulk insert failed at {start}: {r!r}"
    write_s = time.perf_counter() - t0
    t1 = time.perf_counter()
    for j in range(queries):
        vec = [round(((j * (k + 3)) % 89) / 89.0, 4) for k in range(dim)]
        sql = (f"SELECT id, L2_DISTANCE(e, '[{','.join(str(x) for x in vec)}]') AS d "
               f"FROM bench WHERE cat = 'c{j % 10}' ORDER BY d LIMIT 10;")
        r = q(s, sql)
        assert r.get("ok") is True and len(r["rows"]) == 10, f"query {j} failed: {str(r)[:200]}"
    query_s = time.perf_counter() - t1
    s.shutdown()
    t2 = time.perf_counter()
    s.spawn()
    recover_s = time.perf_counter() - t2
    r = q(s, "SELECT id FROM bench ORDER BY id LIMIT 3;")
    assert r["rows"] == [[0], [1], [2]], f"persistence after scale: {r!r}"
    s.shutdown()
    s.close()
    wal_bytes = (d / "wal.log").stat().st_size
    out.append((f"scale {n}x{dim}: write={write_s:.3f}s query{queries}={query_s:.3f}s "
                f"({query_s / queries * 1000:.2f}ms/query) recover={recover_s:.3f}s "
                f"wal={wal_bytes / 1048576:.1f}MiB", "PASS", ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", default=str(ROOT / "build" / "minivectordb.exe"))
    ap.add_argument("--output", default=str(ROOT / "local-results" / "extra"))
    ap.add_argument("--scale", type=int, default=10000)
    ap.add_argument("--queries", type=int, default=200)
    args = ap.parse_args()
    exe = Path(args.exe)
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    d = Path(tempfile.mkdtemp(prefix="mvdb-extra-"))
    s = Session(str(exe), str(d), out / "boundary.stderr.log")
    try:
        boundary_tests(s, rows)
        sql_feature_tests(s, rows)
        s.shutdown()
    finally:
        s.close()
    crash_tests(exe, out, rows)
    scale_test(exe, out, rows, n=args.scale, queries=args.queries)
    for label, status, extra in rows:
        print(f"{status}: {label} {extra}")
    failed = [r for r in rows if r[1] != "PASS"]
    (out / "extra.json").write_text(json.dumps(rows, ensure_ascii=False, indent=2) + "\n")
    print(f"\n{'ALL PASS' if not failed else 'FAILURES'}: {len(rows) - len(failed)}/{len(rows)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
