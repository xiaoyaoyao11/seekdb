"""Local recording protocol fixture for native AI SQL regression tests.

This is NOT the organizer's Mock AI service or a real model. It provides explicit,
synthetic protocol responses; production code must never depend on these values.
Use --port 8000 when running the unchanged public mysqltest on an isolated DB.
Only loopback binding is supported. Authorization headers are never logged.
"""

import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import threading
from typing import cast


class ProtocolServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, port, request_log=None):
        super().__init__(("127.0.0.1", port), ProtocolHandler)
        self.request_log = request_log
        self.log_lock = threading.Lock()

    def record(self, path, payload):
        if self.request_log is not None:
            with self.log_lock, self.request_log.open("a", encoding="utf-8") as stream:
                stream.write(json.dumps({"path": path, "body": payload}, ensure_ascii=False) + "\n")


def embeddings(inputs, dimension):
    if isinstance(inputs, str):
        inputs = [inputs]
    if (not isinstance(inputs, list) or not inputs or
            not all(isinstance(text, str) for text in inputs)):
        raise ValueError("input must contain one or more strings")
    if type(dimension) is not int or dimension <= 0 or dimension > 8192:
        raise ValueError("invalid test-fixture vector dimension")
    vectors = []
    for text in inputs:
        seed = int.from_bytes(hashlib.sha256(text.encode()).digest()[:2], "big")
        vectors.append([((seed + position) % 997) / 997.0 for position in range(dimension)])
    return vectors


class ProtocolHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self):
        self.connection.settimeout(15)
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 16 * 1024 * 1024:
                raise ValueError("invalid body length")
            payload = json.loads(self.rfile.read(length))
            if not isinstance(payload, dict):
                raise ValueError("request must be an object")
            cast(ProtocolServer, self.server).record(self.path, payload)
            if self.path.endswith("/chat/completions"):
                messages = payload.get("messages")
                if not isinstance(messages, list) or not messages:
                    raise ValueError("messages missing")
                text = messages[-1].get("content")
                if not isinstance(text, str):
                    raise ValueError("message content must be text")
                result = {"choices": [{"index": 0, "message": {
                    "role": "assistant", "content": "recorded:" + text}, "finish_reason": "stop"}]}
            elif self.path.endswith("/embeddings"):
                vectors = embeddings(payload.get("input"), payload.get("dimensions", 768))
                # Deliberately reverse items: the native implementation must honor indices.
                result = {"data": [{"index": index, "embedding": vector, "object": "embedding"}
                                   for index, vector in reversed(list(enumerate(vectors)))]}
            elif self.path.endswith("/rerank"):
                documents = payload.get("documents")
                if not isinstance(documents, list) or not documents:
                    raise ValueError("documents missing")
                result = {"results": [{"index": index, "relevance_score": 1.0 / (index + 1),
                                       "document": {"text": document}}
                                      for index, document in reversed(list(enumerate(documents)))]}
            elif self.path == "/api/generate":
                if payload.get("stream") is not False:
                    raise ValueError("native Ollama non-streaming request required")
                text = payload.get("prompt")
                if not isinstance(text, str):
                    raise ValueError("prompt missing")
                result = {"response": "recorded:" + text, "done": True}
            elif self.path == "/api/embed":
                result = {"embeddings": embeddings(payload.get("input"), payload.get("dimensions", 768))}
            else:
                self.reply(404, {"error": "unsupported fixture endpoint"})
                return
            self.reply(200, result)
        except (ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
            self.reply(400, {"error": str(error)})

    def reply(self, status, value):
        data = json.dumps(value, ensure_ascii=False, allow_nan=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, format, *args):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--request-log", type=Path)
    args = parser.parse_args()
    if args.request_log is not None:
        args.request_log.parent.mkdir(parents=True, exist_ok=True)
    server = ProtocolServer(args.port, args.request_log)
    print(f"RECORDING_PROTOCOL_FIXTURE_READY 127.0.0.1:{server.server_port}", flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
