# AI function enhancement regressions

The complete modified C++ server has now been built and exercised through real
SQL. Local suites passed: constructor 15, rendering/completion 16, embedding 11,
Ollama 11, rerank 7. The unchanged public `ai_prompt` and
`ai_function_enhancement` mysqltests also passed with their original expected
results. They used the explicitly synthetic `ai_protocol_fixture.py`, not an
organizer-hosted Mock AI service. No official platform score is claimed.

The earlier helper-only checkpoint is retained below for historical context;
the pending items listed there have since been implemented and SQL-verified.

## Historical helper checkpoint

This checkpoint fixes prompt-object validation and template rendering helpers. It is **not a completed AI_PROMPT competition submission**.

## Verified

The standalone native suite compiles the full real `ob_ai_func_utils.cpp` and links real seekdb libraries:

- Changed helper: 34 cases passed, 0 failed, 0 setup blockers.
- Original helper: 26 passed, 8 failed, 0 setup blockers.
- Regressions cover repeated and structured arguments, nested envelopes, malformed placeholders, integer overflow, byte preservation and allocation failures.

Run inside Linux/WSL after configuring a compatible seekdb build and building its core libraries:

```sh
python tests/competition/run_ai_prompt_renderer_native.py \
  --build-dir /path/to/seekdb/build_release \
  --source-root /path/to/source/under/test
```

The runner records exact commands, source SHA-256 and results in the build directory. It does not replace building/testing the complete server.

## Full-server SQL verification

The constructor's JSON Array/Object gates, TEXT/LONGTEXT materialization,
completion rendering/config and all provider paths have now been exercised on
the rebuilt server. Local success does not establish an official platform score.

## SQL regression execution

Use a fresh isolated test instance. `mysql_port_mode=disabled` allows a Unix socket without exposing a passwordless SQL TCP listener. Set `SEEKDB_TEST_SOCKET` to its `run/sql.sock`, then run:

```sh
python tests/competition/test_ai_prompt_integration.py --first
python tests/competition/test_ai_prompt_integration.py -v
python tests/competition/test_ai_prompt_rendering.py -v
python tests/competition/test_ai_embed_integration.py -v
python tests/competition/test_ai_ollama_integration.py -v
python tests/competition/test_ai_rerank_integration.py -v
```

For the unchanged public tests, start `ai_protocol_fixture.py --port 8000` in the
same namespace, then use `run_ai_public_mysqltests.py --source-root <repo>
--mysqltest <bundled-mysqltest> --socket <socket> --output-dir <results>`.
The runner copies test/result bytes from Git HEAD and verifies hashes without
using `--record`. Use a dedicated database: public tests use fixed resource
names. Stop the fixture and dedicated database when finished; they are services
and do not stop automatically when a test exits.

The rendering suite uses a loopback-only recording protocol fixture and asserts actual request contents. It does not call a real model or replace the official Mock AI service. SQL tests require PyMySQL; connection/model-registration failures are environment blockers, not semantic regression evidence.
