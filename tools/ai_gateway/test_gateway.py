#!/usr/bin/env python3
"""Deterministic OpenAI-compatible fake upstream used by the AI column tests.

The gateway is intentionally dependency free (Python standard library only) so
that it can be started on any machine without a real model provider.

Supported endpoints
-------------------
  POST /v1/chat/completions   chat completion (AI_COMPLETE)
  POST /v1/embeddings         dense embedding  (AI_EMBED)
  GET  /stats                 aggregated usage statistics (JSON)
  GET  /healthz               liveness probe

Fault injection
---------------
The behaviour is driven by the request content, matching the acceptance tests:

  path contains "always-fail"     -> HTTP 400 for every attempt
  user text contains "retry-once" -> HTTP 400 on the first attempt, 200 after
  user text contains "slow-old"   -> sleep ``--slow-delay`` seconds (default 1.2)
  other requests                  -> sleep ``--delay`` seconds (default 0.05)

Embedding values are deterministic and mirror the acceptance fixtures:

  "vec-zero"   -> [0, 0, 0, 0]
  "vec-ten"    -> [10, 0, 0, 0]
  "vec-twenty" -> [20, 0, 0, 0]
  otherwise    -> [index + 1, 0.25, 0.5, 0.75]

Usage accounting
----------------
Every request is appended to ``<state-dir>/events.jsonl`` and accumulated in
memory (requests, attempts, failures, prompt/completion token estimates).  This
implements the "model invocation cost accounting" requirement of the task.

Usage
-----
  python3 test_gateway.py serve   --port 19120
  python3 test_gateway.py start   --port 19120
  python3 test_gateway.py stop    --port 19120
  python3 test_gateway.py stats   --port 19120

The ``start``/``stop`` sub-commands daemonise the server and are used by the
mysql_test ``--exec`` directives.
"""

import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


DEFAULT_PORT = 19120


def state_dir(port):
    return os.path.join(tempfile.gettempdir(), "seekdb_ai_gateway_%d" % port)


def state_path(port, name):
    return os.path.join(state_dir(port), name)


def read_events(port):
    events = []
    path = state_path(port, "events.jsonl")
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as stream:
            for line in stream:
                line = line.strip()
                if line:
                    events.append(json.loads(line))
    return events


def stop_server(port):
    pid_path = state_path(port, "server.pid")
    if not os.path.exists(pid_path):
        return
    try:
        with open(pid_path, "r", encoding="ascii") as stream:
            pid = int(stream.read().strip())
        if os.name == "nt":
            subprocess.call(["taskkill", "/PID", str(pid), "/F"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            os.kill(pid, signal.SIGTERM)
    except (ValueError, ProcessLookupError, OSError):
        pass
    try:
        os.unlink(pid_path)
    except OSError:
        pass


def start_server(port, delay, slow_delay):
    os.makedirs(state_dir(port), exist_ok=True)
    stop_server(port)
    for name in ("events.jsonl", "server.out"):
        with open(state_path(port, name), "w", encoding="utf-8"):
            pass
    output = open(state_path(port, "server.out"), "a", encoding="utf-8")
    kwargs = {}
    if os.name != "nt":
        kwargs["start_new_session"] = True
    process = subprocess.Popen(
        [sys.executable, os.path.abspath(__file__), "serve",
         "--port", str(port), "--delay", str(delay),
         "--slow-delay", str(slow_delay)],
        stdin=subprocess.DEVNULL,
        stdout=output,
        stderr=subprocess.STDOUT,
        **kwargs)
    with open(state_path(port, "server.pid"), "w", encoding="ascii") as stream:
        stream.write(str(process.pid))
    for _ in range(150):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.05):
                return
        except OSError:
            if process.poll() is not None:
                break
            time.sleep(0.02)
    raise RuntimeError("ai gateway did not start on port %d" % port)


def _collect_texts(body):
    system_text = ""
    user_text = ""
    for message in body.get("messages", []) or []:
        if message.get("role") == "system":
            system_text = str(message.get("content", ""))
        elif message.get("role") == "user":
            user_text = str(message.get("content", ""))
    return system_text, user_text


def serve(port, delay, slow_delay):
    lock = threading.Lock()
    attempts = {}
    stats = {
        "requests": 0,
        "attempts": 0,
        "failures": 0,
        "completion_tokens": 0,
        "prompt_tokens": 0,
        "embedding_requests": 0,
    }
    log_path = state_path(port, "events.jsonl")

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            return

        def _json(self, status, payload):
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            parsed = urlparse(self.path)
            if parsed.path.endswith("/stats"):
                with lock:
                    snapshot = dict(stats)
                snapshot["events"] = len(read_events(port))
                self._json(200, snapshot)
            elif parsed.path.endswith("/healthz"):
                self._json(200, {"status": "ok"})
            else:
                self._json(404, {"error": {"message": "not found"}})

        def do_POST(self):
            size = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(size) if size > 0 else b"{}"
            try:
                body = json.loads(raw.decode("utf-8"))
            except Exception:
                body = {}
            parsed = urlparse(self.path)
            system_text, user_text = _collect_texts(body)
            is_embedding = parsed.path.endswith("/embeddings")
            if is_embedding:
                values = body.get("input", "")
                if isinstance(values, list):
                    user_text = "|".join(str(value) for value in values)
                else:
                    user_text = str(values)
            key = parsed.path + "|" + user_text
            with lock:
                attempts[key] = attempts.get(key, 0) + 1
                attempt = attempts[key]
                stats["requests"] += 1
                stats["attempts"] += 1
                if is_embedding:
                    stats["embedding_requests"] += 1
                stats["prompt_tokens"] += max(1, len(system_text + user_text) // 4)

            time.sleep(slow_delay if "slow-old" in user_text else delay)

            fail = "always-fail" in parsed.path or (
                "retry-once" in user_text and attempt == 1)
            if fail:
                status = 400
                response = {"error": {"message": "injected failure"}}
            elif is_embedding:
                inputs = body.get("input", [])
                if not isinstance(inputs, list):
                    inputs = [inputs]

                def vector(value, index):
                    text = str(value)
                    if "vec-zero" in text:
                        return [0.0, 0.0, 0.0, 0.0]
                    if "vec-ten" in text:
                        return [10.0, 0.0, 0.0, 0.0]
                    if "vec-twenty" in text:
                        return [20.0, 0.0, 0.0, 0.0]
                    return [float(index + 1), 0.25, 0.5, 0.75]

                status = 200
                response = {
                    "object": "list",
                    "data": [
                        {"object": "embedding", "index": index,
                         "embedding": vector(value, index)}
                        for index, value in enumerate(inputs)
                    ],
                    "model": body.get("model", "mock-embed"),
                }
            else:
                if "base-10 integer" in system_text:
                    content = "7"
                elif "valid JSON value" in system_text:
                    content = json.dumps({"category": "mock", "input": user_text},
                                         ensure_ascii=False)
                else:
                    content = "mock:" + user_text
                status = 200
                response = {
                    "object": "chat.completion",
                    "model": body.get("model", "mock-complete"),
                    "choices": [{
                        "index": 0,
                        "message": {"role": "assistant", "content": content},
                        "finish_reason": "stop",
                    }],
                    "usage": {"prompt_tokens": 1, "completion_tokens": 1,
                              "total_tokens": 2},
                }

            with lock:
                if status != 200:
                    stats["failures"] += 1
                else:
                    stats["completion_tokens"] += max(1, len(str(response)) // 4)
                event = {
                    "time": time.time(),
                    "path": parsed.path,
                    "status": status,
                    "attempt": attempt,
                    "authorization": self.headers.get("Authorization", ""),
                    "system": system_text,
                    "user": user_text,
                }
                with open(log_path, "a", encoding="utf-8") as stream:
                    stream.write(json.dumps(event, ensure_ascii=False) + "\n")
            self._json(status, response)

    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["serve", "start", "stop", "stats"])
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--delay", type=float, default=0.05)
    parser.add_argument("--slow-delay", type=float, default=1.2)
    args = parser.parse_args()

    if args.mode == "serve":
        serve(args.port, args.delay, args.slow_delay)
    elif args.mode == "start":
        start_server(args.port, args.delay, args.slow_delay)
    elif args.mode == "stop":
        stop_server(args.port)
    else:
        snapshot = {"events": read_events(args.port)}
        print(json.dumps(snapshot, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
