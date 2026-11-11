/* Compare Gated DeltaNet output and snapshot state across two column layouts.
 */
#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define S 128u
#define HV 32u
#define HK 16u
#define RUNS 5
static void ck(cl_int e, const char *s) {
  if (e != CL_SUCCESS) {
    fprintf(stderr, "%s: %d\n", s, e);
    exit(1);
  }
}
#define CK(x) ck((x), #x)
static void *mem(size_t n) {
  void *p = malloc(n);
  if (!p) {
    perror("malloc");
    exit(1);
  }
  return p;
}
static char *readfile(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long z = ftell(f);
  if (z < 1) {
    exit(1);
  }
  rewind(f);
  char *p = mem((size_t)z + 1);
  if (fread(p, 1, (size_t)z, f) != (size_t)z)
    exit(1);
  fclose(f);
  p[z] = 0;
  *n = (size_t)z;
  return p;
}
static uint32_t rng = 621;
static float randval(void) {
  rng = rng * 1664525u + 1013904223u;
  return ((int)(rng >> 16) - 32768) / 32768.0f;
}
static cl_mem input(cl_context c, const void *p, size_t n) {
  cl_int e;
  cl_mem b = clCreateBuffer(c, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, n,
                            (void *)p, &e);
  ck(e, "input");
  return b;
}
static cl_kernel build(cl_context c, cl_device_id d, const char *file, int cpl,
                       int kda, int shuffle) {
  size_t n;
  char *src = readfile(file, &n);
  cl_int e;
  cl_program p = clCreateProgramWithSource(c, 1, (const char **)&src, &n, &e);
  ck(e, "program");
  char opt[256];
  snprintf(opt, sizeof(opt),
           "-cl-std=CL2.0 -cl-mad-enable -cl-unsafe-math-optimizations "
           "-cl-finite-math-only -cl-fast-relaxed-math -DS_V=128 -DKDA=%d "
           "-DSUBGROUP_SIZE=64 -DLANES_PER_COLUMN=8 -DCOLS_PER_LANE_GROUP=%d "
           "-DSUBGROUPS_PER_WG=1 -DUSE_QCOM_SUBGROUP_SHUFFLE=%d",
           kda, cpl, shuffle);
  e = clBuildProgram(p, 1, &d, opt, NULL, NULL);
  if (e != CL_SUCCESS) {
    size_t z = 0;
    clGetProgramBuildInfo(p, d, CL_PROGRAM_BUILD_LOG, 0, NULL, &z);
    char *log = mem(z + 1);
    clGetProgramBuildInfo(p, d, CL_PROGRAM_BUILD_LOG, z, log, NULL);
    log[z] = 0;
    fprintf(stderr, "%s\n", log);
    exit(1);
  }
  free(src);
  cl_kernel k = clCreateKernel(p, "kernel_gated_delta_net", &e);
  ck(e, "kernel");
  CK(clReleaseProgram(p));
  return k;
}
static void args(cl_kernel k, cl_mem *in, cl_mem out, cl_uint t, cl_uint snaps,
                 int kda) {
  cl_uint idx = 0;
  cl_ulong zero = 0;
  for (int i = 0; i < 6; i++) {
    CK(clSetKernelArg(k, idx++, sizeof(cl_mem), &in[i]));
    CK(clSetKernelArg(k, idx++, sizeof(zero), &zero));
  }
  CK(clSetKernelArg(k, idx++, sizeof(out), &out));
  CK(clSetKernelArg(k, idx++, sizeof(zero), &zero));
  cl_uint hv = HV, nseq = 1, soff = S * HV * t, sq1 = S, sq2 = S * HK,
          sq3 = sq2 * t, sv1 = S, sv2 = S * HV, sv3 = sv2 * t, sb1 = 1,
          sb2 = HV, sb3 = HV * t, hk = HK, rq3 = 1;
  float scale = 1.0f / sqrtf((float)S);
  (void)kda;
  cl_uint *a[] = {&hv,  &t,   &nseq, &soff, &sq1, &sq2, &sq3, &sv1,
                  &sv2, &sv3, &sb1,  &sb2,  &sb3, &hk,  &rq3};
  for (size_t i = 0; i < sizeof(a) / sizeof(a[0]); i++)
    CK(clSetKernelArg(k, idx++, sizeof(cl_uint), a[i]));
  CK(clSetKernelArg(k, idx++, sizeof(scale), &scale));
  CK(clSetKernelArg(k, idx++, sizeof(snaps), &snaps));
}
static uint64_t hash(const float *p, size_t n) {
  const unsigned char *v = (const unsigned char *)p;
  uint64_t h = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < n * sizeof(float); i++) {
    h ^= v[i];
    h *= UINT64_C(1099511628211);
  }
  return h;
}
static double checkref(const float *q, const float *k, const float *v,
                       const float *g, const float *beta, const float *state,
                       const float *out, cl_uint t, cl_uint snaps, int kda) {
  double maxerr = 0;
  const size_t ss = (size_t)S * S * HV, soff = (size_t)S * HV * t;
  float scale = 1.0f / sqrtf((float)S);
  for (unsigned h = 0; h < 2; h++)
    for (unsigned col = 0; col < 8; col++) {
      float s[S];
      size_t sb = (size_t)h * S * S + (size_t)col * S;
      memcpy(s, state + sb, sizeof(s));
      for (unsigned ti = 0; ti < t; ti++) {
        size_t qoff = (size_t)ti * S * HK + (h % HK) * S,
               voff = (size_t)ti * S * HV + h * S + col,
               gb = (size_t)ti * HV + h;
        float beta0 = beta[gb], sum = 0;
        if (kda) {
          for (unsigned i = 0; i < S; i++)
            sum += (expf(g[gb * S + i]) * s[i]) * k[qoff + i];
        } else {
          for (unsigned i = 0; i < S; i++)
            sum += s[i] * k[qoff + i];
          sum *= expf(g[gb]);
        }
        float delta = (v[voff] - sum) * beta0, attn = 0;
        for (unsigned i = 0; i < S; i++) {
          float gv = expf(g[kda ? gb * S + i : gb]);
          s[i] = gv * s[i] + k[qoff + i] * delta;
          attn += s[i] * q[qoff + i];
        }
        size_t outidx = (size_t)ti * S * HV + h * S + col;
        double e = fabs((double)out[outidx] - attn * scale);
        if (!isfinite(e))
          return INFINITY;
        if (e > maxerr)
          maxerr = e;
        if (snaps > 1 && t - 1 - ti < snaps) {
          size_t pos = soff + (size_t)(t - 1 - ti) * ss + sb;
          for (unsigned i = 0; i < S; i++) {
            e = fabs((double)out[pos + i] - s[i]);
            if (!isfinite(e))
              return INFINITY;
            if (e > maxerr)
              maxerr = e;
          }
        }
      }
      if (snaps == 1) {
        size_t pos = soff + sb;
        for (unsigned i = 0; i < S; i++) {
          double e = fabs((double)out[pos + i] - s[i]);
          if (!isfinite(e))
            return INFINITY;
          if (e > maxerr)
            maxerr = e;
        }
      }
    }
  return maxerr;
}
int main(int argc, char **argv) {
  if (argc != 7) {
    fprintf(stderr,
            "usage: %s BASE CANDIDATE TOKENS SNAPSHOTS KDA CANDIDATE_CPL\n",
            argv[0]);
    return 2;
  }
  cl_uint t = (cl_uint)atoi(argv[3]), snaps = (cl_uint)atoi(argv[4]);
  int kda = atoi(argv[5]);
  if (t < 1 || t > 256 || snaps < 1 || snaps > 4 || (kda != 0 && kda != 1))
    return 2;
  int cpl = t == 1 ? 1 : 4;
  int candidate_cpl = atoi(argv[6]);
  if (candidate_cpl != 1 && candidate_cpl != 2 && candidate_cpl != 4 &&
      candidate_cpl != 8)
    return 2;
  size_t qn = (size_t)S * HK * t, vn = (size_t)S * HV * t,
         gn = (size_t)HV * t * (kda ? S : 1), bn = (size_t)HV * t,
         sn = (size_t)S * S * HV, on = vn + sn * snaps;
  float *q = mem(qn * 4), *k = mem(qn * 4), *v = mem(vn * 4), *g = mem(gn * 4),
        *beta = mem(bn * 4), *state = mem(sn * 4), *a = calloc(on, 4),
        *b = calloc(on, 4);
  if (!a || !b)
    return 1;
  for (size_t i = 0; i < qn; i++) {
    q[i] = randval() * 0.08f;
    k[i] = randval() * 0.08f;
  }
  for (size_t i = 0; i < vn; i++)
    v[i] = randval() * 0.2f;
  for (size_t i = 0; i < gn; i++)
    g[i] = -0.05f - fabsf(randval()) * 0.05f;
  for (size_t i = 0; i < bn; i++)
    beta[i] = 0.3f + fabsf(randval()) * 0.4f;
  for (size_t i = 0; i < sn; i++)
    state[i] = randval() * 0.01f;
  cl_platform_id plat;
  cl_device_id dev;
  CK(clGetPlatformIDs(1, &plat, NULL));
  CK(clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 1, &dev, NULL));
  size_t exn = 0;
  CK(clGetDeviceInfo(dev, CL_DEVICE_EXTENSIONS, 0, NULL, &exn));
  char *ext = mem(exn + 1);
  CK(clGetDeviceInfo(dev, CL_DEVICE_EXTENSIONS, exn, ext, NULL));
  ext[exn] = 0;
  printf("qcom_shuffle=%d tokens=%u snapshots=%u kda=%d base_cpl=%d "
         "candidate_cpl=%d\n",
         strstr(ext, "cl_qcom_subgroup_shuffle") != NULL, t, snaps, kda, cpl,
         candidate_cpl);
  free(ext);
  cl_int e;
  cl_context ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &e);
  ck(e, "context");
  const cl_queue_properties props[] = {CL_QUEUE_PROPERTIES,
                                       CL_QUEUE_PROFILING_ENABLE, 0};
  cl_command_queue cq = clCreateCommandQueueWithProperties(ctx, dev, props, &e);
  ck(e, "queue");
  cl_kernel base = build(ctx, dev, argv[1], cpl, kda, 1),
            cand = build(ctx, dev, argv[2], candidate_cpl, kda, 1);
  cl_mem in[] = {input(ctx, q, qn * 4),    input(ctx, k, qn * 4),
                 input(ctx, v, vn * 4),    input(ctx, g, gn * 4),
                 input(ctx, beta, bn * 4), input(ctx, state, sn * 4)};
  cl_mem outa = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                               on * 4, a, &e);
  ck(e, "outa");
  cl_mem outb = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                               on * 4, b, &e);
  ck(e, "outb");
  args(base, in, outa, t, snaps, kda);
  args(cand, in, outb, t, snaps, kda);
  const size_t local[] = {64, 1, 1};
  for (int round = 0; round < 3; round++) {
    cl_kernel x = round == 1 ? cand : base;
    const size_t global[] = {
        HV * 64, 1, S / (8 * (unsigned)(round == 1 ? candidate_cpl : cpl))};
    printf("%s_ms=", round == 1   ? "candidate"
                     : round == 0 ? "before"
                                  : "after");
    for (int i = 0; i < RUNS; i++) {
      cl_event ev;
      CK(clEnqueueNDRangeKernel(cq, x, 3, NULL, global, local, 0, NULL, &ev));
      CK(clWaitForEvents(1, &ev));
      cl_ulong a0, z0;
      CK(clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_START, sizeof(a0),
                                 &a0, NULL));
      CK(clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_END, sizeof(z0), &z0,
                                 NULL));
      CK(clReleaseEvent(ev));
      printf("%s%.4f", i ? "," : "", (double)(z0 - a0) / 1e6);
    }
    putchar('\n');
  }
  CK(clEnqueueReadBuffer(cq, outa, CL_TRUE, 0, on * 4, a, 0, NULL, NULL));
  CK(clEnqueueReadBuffer(cq, outb, CL_TRUE, 0, on * 4, b, 0, NULL, NULL));
  size_t diff = 0;
  for (size_t i = 0; i < on; i++)
    if (memcmp(a + i, b + i, 4))
      diff++;
  double err = checkref(q, k, v, g, beta, state, a, t, snaps, kda);
  printf("base_hash=%016llx candidate_hash=%016llx differing_floats=%zu "
         "cpu_max_abs=%.9g\n",
         (unsigned long long)hash(a, on), (unsigned long long)hash(b, on), diff,
         err);
  size_t untouched_bad = 0;
  if (snaps > t) {
    size_t first = vn + sn * t;
    for (size_t i = first; i < on; i++)
      if (a[i] != 0.0f || b[i] != 0.0f)
        untouched_bad++;
  }
  printf("unwritten_snapshot_errors=%zu\n", untouched_bad);
  int result = diff || err > 0.002 || untouched_bad ? 1 : 0;
  CK(clReleaseMemObject(outa));
  CK(clReleaseMemObject(outb));
  for (int i = 0; i < 6; i++)
    CK(clReleaseMemObject(in[i]));
  CK(clReleaseKernel(base));
  CK(clReleaseKernel(cand));
  CK(clReleaseCommandQueue(cq));
  CK(clReleaseContext(ctx));
  free(q);
  free(k);
  free(v);
  free(g);
  free(beta);
  free(state);
  free(a);
  free(b);
  return result;
}
