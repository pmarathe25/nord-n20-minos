/* Nord N20 C.24 OpenCL 2.0 compatibility for llama.cpp b12110. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <string.h>

typedef void *(*create_program_fn)(void *, unsigned, const char **, const size_t *, int *);

void *clCreateProgramWithSource(void *context, unsigned count, const char **strings,
                                const size_t *lengths, int *error) {
    create_program_fn create = (create_program_fn)dlsym(RTLD_NEXT, "clCreateProgramWithSource");
    if (!create) {
        if (error)
            *error = -59; /* CL_INVALID_OPERATION */
        return NULL;
    }

    /* The stock compiler lacks this packed dot-product intrinsic. Preserve
       its signed byte and saturating-add semantics using OpenCL 2.0 ops. */
    static const char name[] = "dot_acc_sat_4x8packed_ss_int";
    static const char fallback[] =
        "inline int dot_acc_sat_4x8packed_ss_int(uint a, uint b, int sum) {\n"
        "  int4 x = convert_int4(as_char4(a));\n"
        "  int4 y = convert_int4(as_char4(b));\n"
        "  int4 p = x * y;\n"
        "  return add_sat(sum, p.s0 + p.s1 + p.s2 + p.s3);\n"
        "}\n";
    if (strings && count <= 32) {
        const char *sources[33] = { fallback };
        size_t sizes[33] = { sizeof(fallback) - 1 };
        int needed = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (!strings[i])
                return create(context, count, strings, lengths, error);
            const size_t size = lengths && lengths[i] ? lengths[i] : strlen(strings[i]);
            sources[i + 1] = strings[i];
            sizes[i + 1] = size;
            needed |= memmem(strings[i], size, name, sizeof(name) - 1) != NULL;
        }
        if (needed)
            return create(context, count + 1, sources, sizes, error);
    }
    return create(context, count, strings, lengths, error);
}
