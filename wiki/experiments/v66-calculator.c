/* Minimal FastRPC round trip. Build once for the DSP and once for Android. */
#include <stddef.h>
#include <stdint.h>

union rpc_arg {
    struct {
        void *data;
        size_t size;
    } buffer;
    uint64_t handle;
};

enum {
    OPEN_CALL = 0x00020001,
    CLOSE_CALL = 0x01000010,
    ADD_CALL = 0x02010100,
};

#ifdef __hexagon__

_Static_assert(sizeof(union rpc_arg) == 8, "Hexagon FastRPC argument layout");

int n20_calculator_skel_handle_invoke(uint64_t handle, uint32_t call, union rpc_arg *args) {
    const uint64_t calculator = 1;
    switch (call) {
    case OPEN_CALL:
        args[2].handle = calculator;
        return 0;
    case CLOSE_CALL:
        return args[0].handle == calculator ? 0 : -1;
    case ADD_CALL:
        if (handle != calculator || args[0].buffer.size != 2 * sizeof(uint32_t) ||
            args[1].buffer.size != sizeof(uint32_t))
            return -1;
        const uint32_t *input = args[0].buffer.data;
        uint32_t *output = args[1].buffer.data;
        *output = input[0] + input[1];
        return 0;
    default:
        return -1;
    }
}

#else

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(union rpc_arg) == 16, "ARM64 FastRPC argument layout");

int main(int argc, char **argv) {
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--library-only") != 0))
        return 2;
    void *library = dlopen("libcdsprpc.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    int (*control)(uint32_t, void *, uint32_t) = dlsym(library, "remote_session_control");
    int (*open)(const char *, uint64_t *) = dlsym(library, "remote_handle64_open");
    int (*invoke)(uint64_t, uint32_t, union rpc_arg *) = dlsym(library, "remote_handle64_invoke");
    int (*close)(uint64_t) = dlsym(library, "remote_handle64_close");
    if (!control || !open || !invoke || !close) {
        fputs("Missing FastRPC client symbols\n", stderr);
        return 1;
    }
    if (argc == 2) {
        puts("C.24 FastRPC client library loaded");
        return 0;
    }

    /* The vendor shell supports an unsigned user process on CDSP domain 3. */
    struct {
        int domain;
        int enable;
    } unsigned_module = {3, 1};
    fputs("FastRPC: request unsigned CDSP session\n", stderr);
    int status = control(2, &unsigned_module, sizeof(unsigned_module));
    if (status) {
        fprintf(stderr, "remote_session_control: 0x%08x\n", (unsigned)status);
        return 1;
    }
    uint64_t handle = 0;
    fputs("FastRPC: open calculator module\n", stderr);
    status = open("file:///libn20_calculator_skel.so?"
                  "n20_calculator_skel_handle_invoke&_modver=1.0&_dom=cdsp", &handle);
    if (status) {
        fprintf(stderr, "remote_handle64_open: 0x%08x\n", (unsigned)status);
        return 1;
    }

    uint32_t cases[][2] = {{17, 25}, {123456789, 987654321}};
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint32_t result = UINT32_MAX;
        union rpc_arg args[] = {
            {.buffer = {cases[i], sizeof(cases[i])}},
            {.buffer = {&result, sizeof(result)}},
        };
        status = invoke(handle, ADD_CALL, args);
        if (status || result != cases[i][0] + cases[i][1]) {
            fprintf(stderr, "remote_handle64_invoke: 0x%08x, result %u\n",
                    (unsigned)status, result);
            failed = 1;
            break;
        }
        printf("CDSP: %u + %u = %u\n", cases[i][0], cases[i][1], result);
    }
    status = close(handle);
    if (status)
        fprintf(stderr, "remote_handle64_close: 0x%08x\n", (unsigned)status);
    return failed || status;
}

#endif
