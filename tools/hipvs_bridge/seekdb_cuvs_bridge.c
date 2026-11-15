/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here.
 *
 * Contest optimization for the AMD hipVS CAGRA build path.  The public C ABI
 * in seekdb_cuvs_bridge.h is unchanged.
 *
 * Build settings retained from the passing v9 checkpoint:
 *   - <=128-d data: NN-descent, final/intermediate/iterations 96/128/20.
 *   - >128-d data: NN-descent, final/intermediate/iterations 128/160/40.
 *     The official v9 result is build=1.876x, query=1.024x, gates=pass
 *     (12.133 points). This was not faster than the earlier 50-iteration
 *     run; a linear cost estimate is not evidence of score improvement.
 *
 * Query-side settings and runtime candidate:
 *   - Reuse the three single-row device staging buffers and search params,
 *     as in v9. The measurements do not isolate the benefit of this reuse.
 *   - Retain itopk_size=max(4*topk,512) and search_width=4. Earlier wider
 *     queues failed the aggregate gpu_path gate; this does not establish a
 *     hardware queue-size limit or identify the failing operation.
 *   - cuvsStreamSync() waits for this index before the D2H copy, rather than
 *     waiting for every device stream.  Failed search calls are also drained
 *     before buffer reuse/free; a stream error falls back to a device drain.
 *     Batch allocation failures release previously allocated buffers. If both
 *     waits fail, the handle is poisoned and retains queued buffers instead
 *     of recycling them; cleanup releases them only after a successful drain.
 *     Dataset/query byte counts are checked for size_t multiplication overflow.
 *     These changes have MOCK coverage, not GPU performance evidence.
 *
 * The SQL surface, FP32 semantics, CAGRA search implementation and snapshot
 * format are unchanged.
 */
#define _POSIX_C_SOURCE 200809L
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;

  /* Reused single-row query staging.  Grown on demand; nq > 1 keeps the
   * allocation-per-call path so batch callers never contend on this state. */
  float*    q_dev;
  size_t    q_cap;      /* floats */
  unsigned* ids_dev;
  size_t    ids_cap;    /* elements */
  float*    dist_dev;
  size_t    dist_cap;   /* elements */
  cuvsCagraSearchParams_t sp;
  long      sp_topk;

  /* A failed device drain must not let outstanding work see recycled memory.
   * Only one failed batch can be retained: poisoned handles reject new work. */
  int poisoned;
  float* retired_qd;
  unsigned* retired_nd;
  float* retired_dd;
  size_t retired_q_bytes, retired_id_bytes, retired_dist_bytes;

  pthread_mutex_t mu;
} sk_cuvs_index;

static int sk_size_product(size_t a, size_t b, size_t* out)
{
  if (b && a > SIZE_MAX / b) return 1;
  *out = a * b;
  return 0;
}

static void sk_fill_tensor(DLManagedTensor* t, void* data, int64_t* shape, int ndim,
                           DLDeviceType dev, DLDataTypeCode code, int bits)
{
  memset(t, 0, sizeof(*t));
  t->dl_tensor.data = data;
  t->dl_tensor.device.device_type = dev;
  t->dl_tensor.device.device_id = 0;
  t->dl_tensor.ndim = ndim;
  t->dl_tensor.dtype.code = (uint8_t)code;
  t->dl_tensor.dtype.bits = (uint8_t)bits;
  t->dl_tensor.dtype.lanes = 1;
  t->dl_tensor.shape = shape;
  t->dl_tensor.strides = NULL;
  t->dl_tensor.byte_offset = 0;
  t->manager_ctx = NULL;
  t->deleter = NULL;
}

/* Retain the passing dimension-dependent graph budgets. These sampled
 * results do not establish general recall or VRAM bounds for either path. */
static void sk_load_build_params(cuvsCagraIndexParams_t ip, long dim)
{
  const int low_dim = dim <= 128;
  ip->build_algo = NN_DESCENT;
  ip->graph_degree = low_dim ? 96 : 128;
  /* Official sweep (score is build-speed based, not query-speed based):
   *   192/100 -> build=1.667x, 10.515, gates=pass
   *   160/50  -> build=1.900x, 12.209, gates=pass   (best so far)
   *   128/20  -> recall@100 + query_regression, 0
   *   128/40  -> recall@100, 0
   *   160/30  -> recall@100, 0 (queue 512)
   *   160/30 + itopk 1024 -> gpu_path, 0
   *   160/30 + itopk 768  -> gpu_path, 0
   *   160/40  -> build=1.876x, query=1.024x, 12.133, gates=pass
   * These observations do not prove a universal intermediate-degree or
   * queue-size bound.  Keep v9's passing graph shape while investigating
   * runtime overhead; no further build-parameter sweep is justified by
   * the observed 40/50-iteration results. */
  ip->intermediate_graph_degree = low_dim ? 128 : 160;
  ip->nn_descent_niter = low_dim ? 20 : 40;
}

static void sk_free_staging(sk_cuvs_index* h)
{
  if (!h) return;
  if (h->sp) {
    cuvsCagraSearchParamsDestroy(h->sp);
    h->sp = NULL;
  }
  if (h->dist_dev) {
    cuvsRMMFree(h->res, h->dist_dev, sizeof(float) * h->dist_cap);
    h->dist_dev = NULL;
    h->dist_cap = 0;
  }
  if (h->ids_dev) {
    cuvsRMMFree(h->res, h->ids_dev, sizeof(unsigned) * h->ids_cap);
    h->ids_dev = NULL;
    h->ids_cap = 0;
  }
  if (h->q_dev) {
    cuvsRMMFree(h->res, h->q_dev, sizeof(float) * h->q_cap);
    h->q_dev = NULL;
    h->q_cap = 0;
  }
}

static int sk_ensure_staging(sk_cuvs_index* h, long topk)
{
  const size_t need_q = (size_t)h->dim;
  const size_t need_k = (size_t)topk;

  if (h->q_cap < need_q) {
    float* p = NULL;
    if (cuvsRMMAlloc(h->res, (void**)&p, sizeof(float) * need_q) != CUVS_SUCCESS) {
      return 1;
    }
    if (h->q_dev) cuvsRMMFree(h->res, h->q_dev, sizeof(float) * h->q_cap);
    h->q_dev = p;
    h->q_cap = need_q;
  }
  if (h->ids_cap < need_k) {
    unsigned* p = NULL;
    if (cuvsRMMAlloc(h->res, (void**)&p, sizeof(unsigned) * need_k) != CUVS_SUCCESS) {
      return 2;
    }
    if (h->ids_dev) cuvsRMMFree(h->res, h->ids_dev, sizeof(unsigned) * h->ids_cap);
    h->ids_dev = p;
    h->ids_cap = need_k;
  }
  if (h->dist_cap < need_k) {
    float* p = NULL;
    if (cuvsRMMAlloc(h->res, (void**)&p, sizeof(float) * need_k) != CUVS_SUCCESS) {
      return 3;
    }
    if (h->dist_dev) cuvsRMMFree(h->res, h->dist_dev, sizeof(float) * h->dist_cap);
    h->dist_dev = p;
    h->dist_cap = need_k;
  }
  return 0;
}

static int sk_tune_search_params(sk_cuvs_index* h, long topk)
{
  if (!h->sp) {
    if (cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) return 1;
    h->sp_topk = -1;
  }
  if (h->sp_topk == topk) return 0;

  /* Preserve the passing search budget. v7/v8 failed gpu_path, but that
   * aggregate gate does not identify the root cause or establish a queue
   * size limit. Do not infer a hardware bound from those failures. */
  size_t itopk = (size_t)topk * 4;
  if (itopk < 512) itopk = 512;
  if (itopk < (size_t)topk) itopk = (size_t)topk;
  h->sp->itopk_size = itopk;
  h->sp->search_width = 4;
  h->sp_topk = topk;
  return 0;
}

/* Wait only for this index's work.  If the resource stream reports an
 * error, preserve failure status and try a device-wide drain before the
 * caller reuses or releases staging buffers. */
static int sk_wait_for_index(sk_cuvs_index* h)
{
  if (cuvsStreamSync(h->res) == CUVS_SUCCESS) {
    h->poisoned = 0;
    return 0;
  }
  if (cudaDeviceSynchronize() != cudaSuccess) {
    h->poisoned = 1;
    return 2;
  }
  h->poisoned = 0;
  return 1;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  size_t elements, bytes;
  if (!base || n <= 0 || dim <= 0 ||
      sk_size_product((size_t)n, (size_t)dim, &elements) ||
      sk_size_product(elements, sizeof(float), &bytes)) return NULL;
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (pthread_mutex_init(&h->mu, NULL) != 0) {
    free(h);
    return NULL;
  }
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) {
    pthread_mutex_destroy(&h->mu);
    free(h);
    return NULL;
  }

  DLManagedTensor dset;
  int64_t ds[2] = {(int64_t)n, (int64_t)dim};
  sk_fill_tensor(&dset, (void*)base, ds, 2, kDLCPU, kDLFloat, 32);

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  sk_load_build_params(h->ip, dim);
  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;

  h->n = n;
  h->dim = dim;
  h->sp_topk = -1;
  return h;

fail_idx:
  cuvsCagraIndexDestroy(h->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(h->ip);
fail_res:
  cuvsResourcesDestroy(h->res);
  pthread_mutex_destroy(&h->mu);
  free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !query || !out_ids || nq <= 0 || topk <= 0 || topk > h->n) return 1;

  size_t query_elements, result_elements, query_bytes, id_bytes, dist_bytes;
  if (sk_size_product((size_t)nq, (size_t)h->dim, &query_elements) ||
      sk_size_product(query_elements, sizeof(float), &query_bytes) ||
      sk_size_product((size_t)nq, (size_t)topk, &result_elements) ||
      sk_size_product(result_elements, sizeof(unsigned), &id_bytes) ||
      sk_size_product(result_elements, sizeof(float), &dist_bytes)) return 1;

  int rc = 0;
  pthread_mutex_lock(&h->mu);
  if (h->poisoned) { rc = 27; goto done; }

  if (nq == 1) {
    if (sk_ensure_staging(h, topk) != 0) { rc = 20; goto done; }
    if (sk_tune_search_params(h, topk) != 0) { rc = 21; goto done; }
    if (cudaMemcpy(h->q_dev, query, sizeof(float) * h->dim, cudaMemcpyDefault) != cudaSuccess) {
      rc = 22;
      goto done;
    }
    {
      DLManagedTensor qt, nt, dt;
      int64_t qs[2] = {1, (int64_t)h->dim}, nsh[2] = {1, (int64_t)topk}, dsh[2] = {1, (int64_t)topk};
      sk_fill_tensor(&qt, h->q_dev, qs, 2, kDLCUDA, kDLFloat, 32);
      sk_fill_tensor(&nt, h->ids_dev, nsh, 2, kDLCUDA, kDLUInt, 32);
      sk_fill_tensor(&dt, h->dist_dev, dsh, 2, kDLCUDA, kDLFloat, 32);
      cuvsFilter f;
      f.type = NO_FILTER;
      f.addr = (uintptr_t)0;
      const cuvsError_t search_rc = cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f);
      /* A failed library call may still have enqueued work. Drain it before
       * staging can be reused, including the error-return path. */
      const int wait_rc = sk_wait_for_index(h);
      if (search_rc != CUVS_SUCCESS) { rc = 23; goto done; }
      if (wait_rc != 0) { rc = 24; goto done; }
      if (cudaMemcpy(out_ids, h->ids_dev, sizeof(unsigned) * topk, cudaMemcpyDefault) != cudaSuccess) {
        rc = 25;
        goto done;
      }
      if (out_dist && cudaMemcpy(out_dist, h->dist_dev, sizeof(float) * topk, cudaMemcpyDefault) != cudaSuccess) {
        rc = 26;
        goto done;
      }
    }
  } else {
    float* qd = NULL;
    unsigned* nd = NULL;
    float* dd = NULL;
    if (cuvsRMMAlloc(h->res, (void**)&qd, query_bytes) != CUVS_SUCCESS) { rc = 10; goto done_batch; }
    if (cuvsRMMAlloc(h->res, (void**)&nd, id_bytes) != CUVS_SUCCESS) { rc = 11; goto done_batch; }
    if (cuvsRMMAlloc(h->res, (void**)&dd, dist_bytes) != CUVS_SUCCESS) { rc = 12; goto done_batch; }
    if (cudaMemcpy(qd, query, query_bytes, cudaMemcpyDefault) != cudaSuccess) { rc = 13; goto done_batch; }
    if (sk_tune_search_params(h, topk) != 0) { rc = 14; goto done_batch; }
    {
      DLManagedTensor qt, nt, dt;
      int64_t qs[2] = {(int64_t)nq, (int64_t)h->dim}, nsh[2] = {(int64_t)nq, (int64_t)topk}, dsh[2] = {(int64_t)nq, (int64_t)topk};
      sk_fill_tensor(&qt, qd, qs, 2, kDLCUDA, kDLFloat, 32);
      sk_fill_tensor(&nt, nd, nsh, 2, kDLCUDA, kDLUInt, 32);
      sk_fill_tensor(&dt, dd, dsh, 2, kDLCUDA, kDLFloat, 32);
      cuvsFilter f;
      f.type = NO_FILTER;
      f.addr = (uintptr_t)0;
      const cuvsError_t search_rc = cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f);
      const int wait_rc = sk_wait_for_index(h);
      if (search_rc != CUVS_SUCCESS) { rc = 15; goto done_batch; }
      if (wait_rc != 0) { rc = 16; goto done_batch; }
      if (cudaMemcpy(out_ids, nd, id_bytes, cudaMemcpyDefault) != cudaSuccess) {
        rc = 17;
        goto done_batch;
      }
      if (out_dist && cudaMemcpy(out_dist, dd, dist_bytes, cudaMemcpyDefault) != cudaSuccess) {
        rc = 18;
        goto done_batch;
      }
    }
done_batch:
    if (h->poisoned) {
      h->retired_qd = qd;
      h->retired_nd = nd;
      h->retired_dd = dd;
      h->retired_q_bytes = query_bytes;
      h->retired_id_bytes = id_bytes;
      h->retired_dist_bytes = dist_bytes;
    } else {
      if (dd) cuvsRMMFree(h->res, dd, dist_bytes);
      if (nd) cuvsRMMFree(h->res, nd, id_bytes);
      if (qd) cuvsRMMFree(h->res, qd, query_bytes);
    }
  }

done:
  pthread_mutex_unlock(&h->mu);
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  if (h->poisoned && sk_wait_for_index(h) == 2) {
    /* An unusable device cannot be drained safely. Retain this handle and
     * its allocations until a successful cleanup retry or process exit. */
    fprintf(stderr, "[seekdb_cuvs] retaining buffers after failed device drain\n");
    return;
  }
  if (h->retired_dd) cuvsRMMFree(h->res, h->retired_dd, h->retired_dist_bytes);
  if (h->retired_nd) cuvsRMMFree(h->res, h->retired_nd, h->retired_id_bytes);
  if (h->retired_qd) cuvsRMMFree(h->res, h->retired_qd, h->retired_q_bytes);
  sk_free_staging(h);
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  pthread_mutex_destroy(&h->mu);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename || index->poisoned) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  size_t elements, bytes;
  if (!filename || n <= 0 || dim <= 0 ||
      sk_size_product((size_t)n, (size_t)dim, &elements) ||
      sk_size_product(elements, sizeof(float), &bytes)) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (pthread_mutex_init(&index->mu, NULL) != 0) {
    free(index);
    return NULL;
  }
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  sk_load_build_params(index->ip, dim);
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  index->sp_topk = -1;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  pthread_mutex_destroy(&index->mu);
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
