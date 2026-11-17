#!/usr/bin/env python3
"""Local (Docker-free) equivalent of protocol_check/run.py.

Windows cannot select() on pipes, so the official _transport.py session is not
usable here.  This driver keeps the official assertion logic, the official
manifest/cases and the official --restart / --mode semantics, but talks to a
locally compiled binary through blocking pipes.  It is a development tool only;
the submitted Dockerfile is the real submission boundary.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACK = ROOT / "mysqltest"
sys.path.insert(0, str(ROOT / "protocol_check"))
from _assertions import assert_response  # noqa: E402

CONFIG = json.loads((ROOT / "protocol_check" / "config.json").read_text())


class Session:
    def __init__(self, exe, data_dir, log_path):
        self.exe, self.data_dir = exe, data_dir
        self.log = open(log_path, "wb", buffering=0)
        self.proc = None
        self.seq = 0
        self.spawn()

    def spawn(self):
        self.proc = subprocess.Popen(
            [self.exe, "--data-dir", self.data_dir],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
            bufsize=0,
        )
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("database exited before ready")
        msg = json.loads(line.decode("utf-8"))
        if msg != {"type": "ready"}:
            raise RuntimeError(f"expected ready, got {msg!r}")

    def request(self, sql=None, mode="exact", op="execute"):
        self.seq += 1
        req = {"id": self.seq, "op": op}
        if sql is not None:
            req.update(sql=sql, search_mode=mode)
        self.proc.stdin.write(json.dumps(req, ensure_ascii=False).encode("utf-8") + b"\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("database exited before response")
        resp = json.loads(line.decode("utf-8"))
        if type(resp.get("id")) is not type(req["id"]) or resp["id"] != req["id"]:
            raise RuntimeError(f"response id differs: {resp!r}")
        if type(resp.get("ok")) is not bool:
            raise RuntimeError(f"missing boolean ok: {resp!r}")
        return resp

    def shutdown(self):
        resp = self.request(op="shutdown")
        if resp.get("ok") is not True:
            raise RuntimeError(f"shutdown failed: {resp!r}")
        self.proc.stdin.close()
        self.proc.wait(timeout=30)
        extra = self.proc.stdout.read()
        if extra or self.proc.returncode:
            raise RuntimeError(f"extra stdout or nonzero exit after shutdown: {extra[:200]!r} rc={self.proc.returncode}")
        self.proc = None

    def crash(self):
        """SIGKILL-equivalent: no shutdown, no flush of user-space buffers."""
        if self.proc:
            self.proc.kill()
            self.proc.wait(timeout=10)
            self.proc.stdin.close()
            self.proc.stdout.close()
            self.proc = None

    def close(self):
        if self.proc:
            try:
                self.proc.kill()
                self.proc.wait(timeout=10)
            except Exception:
                pass
        self.log.close()


def run_case(case, exe, out_dir):
    expectations = json.loads((PACK / case["result"]).read_text(encoding="utf-8"))
    statements = (PACK / case["test"]).read_text(encoding="utf-8").splitlines()
    data_dir = Path(tempfile.mkdtemp(prefix="mvdb-" + case["id"] + "-"))
    log = out_dir / (case["id"] + ".stderr.log")
    sess = Session(str(exe), str(data_dir), log)
    index, mode = 0, "exact"
    try:
        for line_no, text in enumerate(statements, 1):
            sql = text.strip()
            if not sql or sql.startswith("#"):
                continue
            if sql == "--restart":
                sess.shutdown()
                sess.spawn()
            elif sql in ("--mode exact", "--mode ann"):
                mode = sql.split()[1]
            elif sql.startswith("--"):
                raise ValueError(f"unsupported directive: {sql}")
            else:
                if index >= len(expectations):
                    raise ValueError("missing expected result")
                resp = sess.request(sql, mode)
                try:
                    assert_response(resp, expectations[index], CONFIG)
                except AssertionError as exc:
                    raise AssertionError(f"line {line_no}: {sql}: {exc} -> {resp!r}") from exc
                index += 1
        if index != len(expectations):
            raise ValueError(f"SQL/result count mismatch: {index} vs {len(expectations)}")
        sess.shutdown()
        return "PASS", ""
    finally:
        sess.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", default=str(ROOT / "build" / "minivectordb.exe"))
    ap.add_argument("--case", action="append")
    ap.add_argument("--handshake-only", action="store_true")
    ap.add_argument("--output", default=str(ROOT / "local-results" / "public"))
    args = ap.parse_args()
    exe = Path(args.exe)
    if not exe.exists():
        ap.error(f"binary not found: {exe}")
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((PACK / "manifest.json").read_text())
    cases = manifest["cases"]
    if args.case:
        unknown = set(args.case) - {c["id"] for c in cases}
        if unknown:
            ap.error("unknown case: " + ", ".join(sorted(unknown)))
        cases = [c for c in cases if c["id"] in args.case]
    rows = []
    if args.handshake_only:
        d = Path(tempfile.mkdtemp(prefix="mvdb-handshake-"))
        s = Session(str(exe), str(d), out / "handshake.stderr.log")
        s.shutdown()
        s.close()
        print("handshake: PASS")
        return 0
    for c in cases:
        try:
            status, msg = run_case(c, exe, out)
        except Exception as exc:
            status, msg = "FAIL", f"{type(exc).__name__}: {exc}"
        rows.append({"id": c["id"], "status": status, "message": msg})
        print(f"{c['id']}: {status} {msg}", flush=True)
    (out / "cases.json").write_text(json.dumps(rows, ensure_ascii=False, indent=2) + "\n")
    passed = all(r["status"] == "PASS" for r in rows)
    print(f"\n{'ALL PASS' if passed else 'FAILURES'}: {sum(r['status']=='PASS' for r in rows)}/{len(rows)}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
