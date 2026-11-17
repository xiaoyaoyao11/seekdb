"""Run unchanged public AI mysqltests with the bundled real mysqltest executable.

Requires a dedicated local seekdb database and the mock/protocol service expected
by the test at port 8000. This runner does NOT imply that service is official.
Original tests/results are read from git and copied byte-for-byte to output-dir;
no --record flag, expected-output edits, or changes to the official test tree.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--mysqltest", required=True, type=Path)
    parser.add_argument("--socket", required=True)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--case", action="append", choices=["ai_function_enhancement", "ai_prompt"])
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = args.source_root.resolve()
    report = {"source_commit": subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip(),
        "service": "external test service; organizer provenance not asserted",
        "socket": args.socket, "cases": []}
    prefix = "tools/deploy/mysql_test/test_suite/ai_function/"
    for name in args.case or ["ai_prompt", "ai_function_enhancement"]:
        work = output / name
        work.mkdir(exist_ok=True)
        inputs = {}
        for suffix, relative in (("test", prefix + "t/" + name + ".test"),
                                 ("result", prefix + "r/mysql/" + name + ".result")):
            data = subprocess.check_output(["git", "-C", str(source), "show", "HEAD:" + relative])
            target = work / (name + "." + suffix)
            target.write_bytes(data)
            inputs[suffix] = {"original": relative, "sha256": hashlib.sha256(data).hexdigest()}
        command = [str(args.mysqltest.resolve()), "--no-defaults", "--user=root",
                   "--socket=" + args.socket, "--database=test", "--default-character-set=utf8mb4",
                   "--connect-timeout=10", "--max-connect-retries=1", "--logdir=" + str(work),
                   "--test-file=" + str(work / (name + ".test")),
                   "--result-file=" + str(work / (name + ".result"))]
        result = subprocess.run(command, cwd=work, capture_output=True, text=True, timeout=240)
        (work / "stdout.log").write_text(result.stdout, encoding="utf-8")
        (work / "stderr.log").write_text(result.stderr, encoding="utf-8")
        entry = {"name": name, "returncode": result.returncode, "inputs": inputs,
                 "argv": command, "stdout": result.stdout, "stderr": result.stderr}
        report["cases"].append(entry)
        print(name + ": exit " + str(result.returncode), flush=True)
        print(result.stdout + result.stderr, flush=True)
        for suffix in ("test", "result"):
            current = (work / (name + "." + suffix)).read_bytes()
            if hashlib.sha256(current).hexdigest() != inputs[suffix]["sha256"]:
                raise RuntimeError("Acceptance input was unexpectedly modified")
    report["all_passed"] = all(case["returncode"] == 0 for case in report["cases"])
    (output / "result.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print("PUBLIC_MYSQLTEST_ALL_PASSED=" + str(report["all_passed"]), flush=True)
    return 0 if report["all_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
