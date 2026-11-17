#!/usr/bin/env python3
"""Pre-submission validation of agent/conversation.json (rule v3-unbounded)."""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATH = ROOT / "agent" / "conversation.json"
LIMIT = 128 * 1024


def reject_duplicates(pairs):
    result = {}
    for k, v in pairs:
        if k in result:
            raise ValueError(f"duplicate JSON key: {k}")
        result[k] = v
    return result


def bad_constant(value):
    raise ValueError(f"non-finite JSON number: {value}")


def resolve(doc, pointer):
    if not pointer.startswith("/"):
        raise ValueError(f"bad pointer: {pointer}")
    cur = doc
    for raw in pointer[1:].split("/"):
        token = raw.replace("~1", "/").replace("~0", "~")
        if isinstance(cur, list):
            cur = cur[int(token)]
        elif isinstance(cur, dict):
            cur = cur[token]
        else:
            raise ValueError(f"pointer traverses a scalar: {pointer}")
    return cur


def main():
    raw = PATH.read_bytes()
    assert len(raw) <= LIMIT, f"file too large: {len(raw)} bytes"
    text = raw.decode("utf-8")
    doc = json.loads(text, object_pairs_hook=reject_duplicates, parse_constant=bad_constant)
    assert isinstance(doc, dict), "top level must be an object"
    assert doc.get("synthetic") is False, "real material must not be marked synthetic"
    assert doc.get("schema") == "agent-learning-evidence-v3-unbounded"

    # No credentials of any kind in the material.
    assert not re.search(r"ghp_[A-Za-z0-9]{20,}", text), "token leaked"
    assert not re.search(r"@github\.com", text), "credential-bearing URL leaked"

    units = doc["evidence_units"]
    dims = set()
    total = 0.0
    for u in units:
        dims.add(u["dimension"])
        tier = u["tier"]
        total += tier
        se, re_ = u["student_evidence"], u["result_evidence"]
        assert se and isinstance(se.get("pointer"), str), f"{u['id']}: missing student pointer"
        sval = resolve(doc, se["pointer"])
        assert isinstance(sval, str), f"{u['id']}: student pointer is not a string"
        assert se["quote"] in sval, f"{u['id']}: student quote not found at {se['pointer']}"
        assert len(se["quote"]) <= 240, f"{u['id']}: student quote too long"
        if tier <= 0.5:
            assert not re_, f"{u['id']}: 0.5-tier must have empty result_evidence"
        else:
            assert re_ and isinstance(re_.get("pointer"), str), f"{u['id']}: missing result pointer"
            rval = resolve(doc, re_["pointer"])
            assert isinstance(rval, str), f"{u['id']}: result pointer is not a string"
            assert re_["quote"] in rval, f"{u['id']}: result quote not found at {re_['pointer']}"
            assert len(re_["quote"]) <= 240, f"{u['id']}: result quote too long"
            assert se["quote"] != re_["quote"], f"{u['id']}: same fragment used twice"
        assert u.get("rationale"), f"{u['id']}: missing rationale"

    expected = {"问题理解与自主拆解", "技术理解与迁移", "独立验证与质疑", "实验与反馈迭代", "取舍与反思"}
    assert dims == expected, f"dimension coverage mismatch: {dims ^ expected}"
    print(f"PASS: {len(raw)} bytes, {len(units)} units, dims={len(dims)}, declared tier sum={total}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"FAIL: {type(exc).__name__}: {exc}")
        sys.exit(1)
