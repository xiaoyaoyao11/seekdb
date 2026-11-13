# hipVS build optimization: verified results and local candidate

## Official results

The following are contest results, not local GPU measurements. On 2026-11-13
the AMD quota was 8 used out of 10 when this local candidate was prepared.

| Configuration, >128 dimensions | Official result | Score |
| --- | --- | ---: |
| NN-descent, final/intermediate/iterations 128/192/100 | build=1.667x, query=0.977x, gates=pass (earlier recorded result) | 10.515 |
| NN-descent, 128/160/50, search queue max(4k,512) | build=1.900x, query=1.005x, gates=pass (earlier recorded result) | 12.209 |
| NN-descent, 128/160/30, search queue max(4k,512) | hidden250k:recall@100_baseline_delta | 0 |
| NN-descent, 128/160/30, search queue max(8k,1024) | gpu_path | 0 |
| NN-descent, 128/160/30, search queue max(6k,768), corrected source | gpu_path | 0 |
| v9: NN-descent, 128/160/40, search queue max(4k,512) | build=1.876x, query=1.024x, gates=pass | 12.133 |

The latest result is task 47252335, commit
`caa3f9e0b2020bad286c063e4be5279f0e4a980d`, branch `amd-cagra-submit`.
The highest recorded score remains 12.209, not 12.133 or 20.

An earlier v8 draft contained an unclosed C comment. Its first submission and
an unnecessary retry failed before completing the test cases. These attempts
must not be treated as evidence about GPU speed or recall.

## What these results do and do not show

- Reducing the high-dimensional descent budget from 50 to 40 did not produce
  a measured build-speed improvement. Previously quoted linear cost models
  and predicted scores are not supported by this comparison and must not be
  used to justify another submission.
- The sampled configurations with intermediate degree 128 failed recall
  gates. This does not prove that every value below 160 must fail.
- `gpu_path` is an aggregate failure message. The two wider-queue failures
  do not establish a universal queue-size limit or identify the failing
  kernel. The passing queue is retained as a conservative checkpoint, not
  described as a hardware limit.
- There is no local Radeon/ROCm performance measurement available for the
  candidate below. Passing MOCK tests is not GPU verification.

## v10 candidate prepared for one authorized official test

All graph-building settings, FP32 tensors, public C ABI, and snapshot data
inclusion stay at the passing v9 checkpoint:

| Dimensions | Build algorithm | Final degree | Intermediate degree | Iterations |
| --- | --- | ---: | ---: | ---: |
| <=128 | NN-descent | 96 | 128 | 20 |
| >128 | NN-descent | 128 | 160 | 40 |

Search retains `itopk_size=max(4*topk,512)` and `search_width=4`.

The local code changes only runtime synchronization and error handling:

1. Replace normal query-path device-wide synchronization with
   `cuvsStreamSync(resources)`. A stream error retains failure status and
   attempts a device-wide drain; it is not silently reported as success.
2. Wait after a failed CAGRA search as well, because a failing call may have
   already queued work before returning.
3. Send every batch allocation failure through batch cleanup, so second- and
   third-allocation failures release the buffers already allocated.
4. Reject missing input, nonpositive build shapes, and top-k beyond the index
   row count before allocating/searching. Check all dataset/query/result byte
   count multiplications for `size_t` overflow before allocation or copying.
5. If both resource and device synchronization fail, poison the handle and
   reject subsequent searches and snapshots. Retain the single-row staging
   buffers or the one failed batch instead of recycling memory referenced by
   outstanding work. Cleanup retries the drain; if it still fails, the handle
   is deliberately retained until a later successful cleanup or process exit.
   This failure-only retention is a safety trade-off, not a normal-path leak.

These changes are a correctness/runtime-overhead candidate. No improvement
in build speed, query throughput, VRAM, recall, or contest score is claimed.

The `cuvsStreamSync(cuvsResources_t)` declaration is documented by AMD here:
<https://rocm.docs.amd.com/projects/hipVS/en/docs-25.10/reference/c_api/core_c_api.html>.
The real project bridge header and `tools/hipvs_bridge/build.sh` were read
from the submitted checkpoint using GitHub CLI. The C ABI matches; that script
uses `hipcc`, `/opt/hipvs/include`, `/opt/rocm/include`, and `-lcuvs_c` (prefixes
are overridable). Actual installed SDK headers/exports and GPU behavior remain
unverified locally; the test SDK headers are deliberately MOCKs.

## Local verification

`hipvs-preflight.py` in the session's `hipvs-review/` directory compiles the
entire bridge with generated MOCK headers in both the AMD and non-AMD include
branches, using C11, `-Wall -Wextra -Werror -Wstrict-prototypes` and
`-Werror=comment`.

The MOCK harness covers single-row and batch queries, repeated staging
reuse, optional distances, all three batch allocation failures, search
failure after queuing, stream failure, double synchronization failure with
poisoned-handle rejection, retained-buffer cleanup after recovery, multiplication
overflow, invalid inputs, and simulated snapshot shape checks. Failed mock waits
now leave work queued rather than silently clearing it. The public `LONG_MAX`
overflow cases run only on LP64 hosts; the Windows test also checks the overflow
helper directly with `SIZE_MAX`. These are host control-flow tests, not real
snapshot-format, GPU correctness, recall, SDK compatibility, or speed tests.

Latest local result for each include branch:

```text
strict_compile_exit=0
MOCK TEST PASS: failures=0 live_buffers=0 resource_syncs=18 global_syncs=5
```

Strict C++17 syntax checks also pass in both MOCK include branches with
`-Wall -Wextra -Werror -Werror=comment`. The MOCK CAGRA types use named
struct tags; the earlier anonymous-type fixture error was corrected without
weakening compiler diagnostics. Deliberately removing a C comment terminator
is rejected in both branches. These extra checks do not compile the real SDK.

All five device-wide sync attempts are deliberate failure injections, including
the cleanup retry while both waits fail. The normal success path does not use
device-wide synchronization.

## Authorized official validation and remaining limitations

The user subsequently authorized actual testing despite the unavailable local
Radeon host. The next step is one official submission of this new v10 candidate,
not a replay of v9. AMD automatic submission remains disabled. Quota, branch,
commit, required fields and cooldown are checked immediately before sending;
the second remaining daily opportunity is not spent until this result is known.
The official test must validate real SDK compilation, GPU routing, snapshots,
recall and performance; none is claimed passed before that result arrives.

For independently reproducible local validation, the missing checks remain:

1. Compile against the actual deployed hipVS/ROCm SDK, without MOCK headers,
   and confirm the required exports.
2. Run GPU/CPU fallback smoke, actual snapshot round-trip, and representative
   recall/VRAM checks. The repository smoke is a 10K x 128 synthetic functional
   test, not the official Hidden250K score protocol.
3. Compare on the same GPU/data with repeated runs, separating upload, graph
   build, serialization and first-query time. Do not derive a predicted score
   from the 40/50-iteration comparisons or treat device-memory samples as peaks.

No new official result or full-score claim exists when this report is prepared.

