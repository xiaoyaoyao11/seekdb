# AI Gateway (fake upstream) for the async AI column tests

`test_gateway.py` is a dependency-free, OpenAI-compatible HTTP server that stands
in for a real model provider while developing and testing the asynchronous
`AI COLUMN` feature. It never contacts a real model, so tests are deterministic
and free.

## Why it exists

The AI column worker calls the same endpoints as `AI_COMPLETE` / `AI_EMBED`:

* `POST /v1/chat/completions` — completion models
* `POST /v1/embeddings` — dense embedding models
* `GET  /stats` — aggregated invocation counters
* `GET  /healthz` — liveness probe

The gateway can inject latency and failures so that the following behaviours can
be verified without a real upstream:

| Scenario | Trigger | Expected engine behaviour |
| --- | --- | --- |
| transient failure | user text contains `retry-once` | worker retries, task state `RETRYING`, result published after success |
| terminal failure | request path contains `always-fail` | retries are exhausted, AI value stays `NULL`, task row is cleaned up |
| stale result | user text contains `slow-old` | the late response must not overwrite a newer generation |
| deterministic embedding | `vec-zero` / `vec-ten` / `vec-twenty` | `[0,0,0,0]` / `[10,0,0,0]` / `[20,0,0,0]` |

## Usage

```bash
# foreground
python3 tools/ai_gateway/test_gateway.py serve --port 19120

# daemon (used by mysql_test --exec directives)
python3 tools/ai_gateway/test_gateway.py start --port 19120
python3 tools/ai_gateway/test_gateway.py stop  --port 19120

# inspect accumulated usage / request history
python3 tools/ai_gateway/test_gateway.py stats --port 19120
curl -s http://127.0.0.1:19120/stats
```

State (pid file, `events.jsonl`, `server.out`) lives in the system temp
directory under `seekdb_ai_gateway_<port>`, so several gateways can run in
parallel on different ports (the acceptance tests use 19120–19124).

## Registering an endpoint

```sql
call DBMS_AI_SERVICE.CREATE_AI_MODEL('mock_complete',
  '{"type":"completion","model_name":"mock-complete"}');
call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('mock_complete_ep',
  '{"ai_model_name":"mock_complete",
    "url":"http://127.0.0.1:19120/v1/chat/completions",
    "access_key":"test-key","provider":"openai",
    "request_model_name":"mock-complete"}');
```

The `access_key` is forwarded as `Authorization: Bearer <key>`; the gateway
records it in `events.jsonl` so tests can assert that credentials were applied.

## Accounting

`GET /stats` returns `requests`, `attempts`, `failures`, `prompt_tokens`,
`completion_tokens` and `embedding_requests`. Token counts are estimates derived
from payload sizes; they are meant for cost observability, not billing.
