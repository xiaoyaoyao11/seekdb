# hipVS build optimization: verified results and local candidate

## Official results

The following are last verified contest results, not scores computed from local
GPU measurements. Daily quota must be queried again before a new submission;
historical usage does not establish the current day's remaining allowance.

| Configuration, >128 dimensions | Official result | Score |
| --- | --- | ---: |
| NN-descent, final/intermediate/iterations 128/192/100 | build=1.667x, query=0.977x, gates=pass (earlier recorded result) | 10.515 |
| NN-descent, 128/160/50, search queue max(4k,512) | build=1.900x, query=1.005x, gates=pass (earlier recorded result) | 12.209 |
| NN-descent, 128/160/30, search queue max(4k,512) | hidden250k:recall@100_baseline_delta | 0 |
| NN-descent, 128/160/30, search queue max(8k,1024) | gpu_path | 0 |
| NN-descent, 128/160/30, search queue max(6k,768), corrected source | gpu_path | 0 |
| v9: NN-descent, 128/160/40, search queue max(4k,512) | build=1.876x, query=1.024x, gates=pass | 12.133 |

The highest last verified result is **v10: 12.259**, task `47252473`, commit
`1e662e7bdf5b3c833893e7dd24c82cc41d3cb92d`, branch `amd-cagra-submit`.
It reported `build=1.886x, query=1.036x, gates=pass` on 2026-11-13.
The small v9/v10 score difference does not isolate a cause or predict a new score.
No full-score claim is made.

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
- Real Radeon/hipVS measurements were subsequently returned from the user's
  existing cloud host, as reported below. Passing MOCK tests alone is not GPU
  verification, and synthetic GPU tests cannot certify an official recall gate.

## Passing v10 checkpoint

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

These were the v10 correctness/runtime-overhead changes. Its recorded official
result is listed above. Neither that single result nor the synthetic comparison
below isolates their benefit or establishes a general performance guarantee.

The `cuvsStreamSync(cuvsResources_t)` declaration is documented by AMD here:
<https://rocm.docs.amd.com/projects/hipVS/en/docs-25.10/reference/c_api/core_c_api.html>.
The real project bridge header and `tools/hipvs_bridge/build.sh` were read
from the submitted checkpoint using GitHub CLI. The C ABI matches; that script
uses `hipcc`, `/opt/hipvs/include`, `/opt/rocm/include`, and `-lcuvs_c` (prefixes
are overridable). Actual cloud SDK headers and GPU execution were subsequently
verified independently, as detailed below; the host regression headers remain
deliberately MOCKs and do not constitute SDK compatibility evidence.

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

## Independent real Radeon / hipVS verification

Two offline batches were returned from the user's existing Radeon Cloud instance
on 2026-11-13 UTC. They used installed `/opt/rocm/bin/hipcc`, `/opt/hipvs/include`,
`/opt/rocm/include`, `-lcuvs_c`, and the real HIP runtime. No MOCK library,
package installation, network request, or contest submission was used.
The deployed CAGRA header SHA256 was
`4fbca2261378ce270dae35ec5aa5675b317fb4374f0e426c3965f75ba9f4d29f`.
Source and compiled-library SHA chains were checked against every report.

The first batch compiled v9/v10 and tested 2048x128, 2048x256, 250Kx128 and
250Kx768 normal synthetic data. IDs were valid and unique, distances were finite
and matched FP32 squared-L2 references, and snapshot ID overlap was 1.0.

| 250K normal | v9 median build | v10 median build | v10 time reduction | v10 min single recall@100 |
| --- | ---: | ---: | ---: | ---: |
| 128 dimensions | 7.330318 s | 7.222099 s | 1.476% | 0.913750 |
| 768 dimensions | 14.152870 s | 14.278876 s | -0.890% | 0.784375 |

There were 16 queries and two repetitions per library. The differences do not
show a reliable large-data build win. Approximately twofold timing differences
on 2048-row smoke cases are not used to infer official performance.

## Fixed high-dimensional parameter comparisons

The second batch contains four successful real-SDK compilations and nine GPU
comparisons. Each candidate changed only high-dimensional intermediate degree
and iteration count; low-dimensional parameters, final degree 128, search, ABI
and safety logic were unchanged. Each had a 2048x256 smoke test and 250Kx768
normal/clustered tests with 64 queries and two alternating repetitions.

| Intermediate / iterations | Distribution | Build-time reduction vs v10 | Mean recall@100 delta (percentage points) | K100 single-latency change |
| --- | --- | ---: | ---: | ---: |
| 160 / 36 | normal | 6.533% | -0.039063 | -0.319% |
| 160 / 36 | clustered | 6.310% | -0.007813 | +0.990% |
| 192 / 30 | normal | 11.728% | -0.750000 | +0.448% |
| 192 / 30 | clustered | 10.560% | +0.015625 | +7.228% |
| 192 / 24 | normal | 20.149% | -0.820313 | +0.229% |
| 192 / 24 | clustered | 19.640% | 0.000000 | +9.955% |

All tests passed ID/distance/snapshot integrity and snapshot overlap was 1.0.
That is **not** an official recall-gate result. The local screen was explicitly
heuristic: at least 1.05x build speedup, K100 latency ratio no more than 1.03,
and mean K10/K100 single/batch recall loss no greater than 0.005. No absolute
recall threshold or official score was computed.

160/36 passed this local screen on both large distributions. Its eight-query
smoke K10 mean recall was 1.25 percentage points below v10, so it is not claimed
to preserve recall universally. 192/30 and 192/24 are rejected because their
normal-data recall loss or clustered query regression exceeds the local screen.

## Prepared v11 candidate: 160/36

The exact source differs from v10 only here:

```c
ip->nn_descent_niter = low_dim ? 20 : 36;
```

Its SHA256 is
`16d75f22568906659fc663d9ddeaf4b7cc43f8ef54fbfdb41a281789ec716267`,
identical to the source compiled and measured on the Radeon host. Strict C11
MOCK compile/control-flow regression passes in AMD and non-AMD include branches
for both v10 and v11; assertions explicitly expect 40 and 36 respectively.

This commit publishes the prepared v11 source for official validation. It has
not yet produced an official result when this report is committed. Immediately
before publication, the original competition account was reauthenticated:
submission options were available, all AMD records were terminal, the branch
still pointed to verified v10, the cooldown was satisfied, and there were zero
AMD records on the current Asia/Shanghai calendar day. Publication itself does
not spend a contest submission.

## Remaining limitations and controlled next step

1. Reauthenticate the original competition account, then verify platform date,
   actual daily usage, no pending task, cooldown, branch and exact commit SHA.
   Publish only the measured candidate and report, then perform at most one
   authorized official test. Never replay an uncertain submission.
2. Local datasets are synthetic. Independent seekdb SQL end-to-end measurement
   and the official hidden-data protocol have not been reproduced.
3. Large tests used two repetitions. Timings include explicit HIP fences and
   cannot directly substitute for official query ratios.
4. GPU memory values are samples, not peaks; host sysfs devices do not prove
   that the user was allocated multiple GPUs.
5. The cloud instance continues billing until the user explicitly stops or
   destroys it. The offline scripts do not change instance state.

No new official result or full-score prediction is claimed.

## v12: host-thread oversubscription fix (round-3 Radeon evidence)

Round-3 offline diagnostics (250K x 256 FP32, real hipVS/ROCm SDK, zero
submissions) isolated a cost that is **not** a graph parameter:

| run | median fenced build | note |
| --- | ---: | --- |
| D1 v10, default OpenMP (128 cores) | 17.254 s | baseline |
| D2 v10, `OMP_NUM_THREADS=2` | 14.103 s | **-18.3%**, identical recall and snapshot |
| D3 v10, RMM pool | 16.610 s | small effect, not adopted |
| D4 v10, `niter=1` | 4.314 s | bridge fixed cost ~4.3 s |

`rocprofv3`: `nn_descent::local_join_kernel` is 73.1% of GPU kernel time and
`cagra::kern_prune` 19.1% (a host-driven optimize phase).  The default 128-way
host team oversubscribes that phase.  Round-3 also ruled out two other ideas:
device-resident input gained only 2.8% (B5) and `ITERATIVE_CAGRA_SEARCH` was
2.1x slower (B6).

v12 restores the exact graph shape of the best official result (v10,
160/40, `itopk=max(4k,512)`, `search_width=4`, `graph_degree=128`) and adds a
build-scoped OpenMP limit: `setenv("OMP_NUM_THREADS","2",0)` for fresh
pthreads, plus `dlopen(RTLD_NOLOAD)` -> `omp_set_num_threads(2)` in the build
thread, restoring the previous value after `cuvsCagraBuild`.  Linux-only and
inert elsewhere.  Strict C11 MOCK compile and control-flow regression pass in
both include branches (`high_iterations=40`).

This is a distinct, evidence-backed change from v11, which failed the official
`hidden250k:query_regression` gate; one official test is therefore authorised.
No full-score claim: the local 18% reduction is diluted by the harness's own
fixed overhead, and the official score is not predicted here.

