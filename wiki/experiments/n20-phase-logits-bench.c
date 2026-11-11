/* End-to-end inference timings and token parity for one prompt and one GPU. */
#define _POSIX_C_SOURCE 200809L
#include "llama.h"
#include "ggml-backend.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { CONTEXT_TOKENS = 1536, MAX_PROMPT_BYTES = 65536, MAX_GENERATION = 128 };

static void require(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static int bounded_int(const char *value, long minimum, long maximum) {
    char *end;
    const long number = strtol(value, &end, 10);
    return *value && !*end && number >= minimum && number <= maximum ? (int)number : -1;
}

static double seconds(void) {
    struct timespec now;
    require(clock_gettime(CLOCK_MONOTONIC, &now) == 0, "clock_gettime failed");
    return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

static int read_tokens(const struct llama_vocab *vocab, const char *path, llama_token *tokens) {
    char *prompt = malloc(MAX_PROMPT_BYTES);
    require(prompt != NULL, "Could not allocate prompt");
    FILE *file = fopen(path, "rb");
    require(file != NULL, "Could not open prompt");
    const size_t bytes = fread(prompt, 1, MAX_PROMPT_BYTES, file);
    require(!ferror(file) && bytes > 0 && bytes < MAX_PROMPT_BYTES, "Invalid prompt length");
    fclose(file);
    const int count = llama_tokenize(vocab, prompt, (int)bytes, tokens, CONTEXT_TOKENS, true, true);
    free(prompt);
    require(count > 0, "Could not tokenize prompt within the context limit");
    return count;
}

static llama_token greedy(struct llama_context *context, int vocab_size, bool on_backend,
                          const llama_token *options, int option_count) {
    if (on_backend) {
        const llama_token token = llama_get_sampled_token_ith(context, -1);
        require(token >= 0 && token < vocab_size, "No valid backend sample");
        return token;
    }
    const float *logits = llama_get_logits_ith(context, -1);
    require(logits != NULL, "No logits for sampling");
    if (option_count) {
        llama_token best = options[0];
        for (int i = 1; i < option_count; ++i) {
            if (logits[options[i]] > logits[best])
                best = options[i];
        }
        return best;
    }
    llama_token best = 0;
    for (llama_token i = 1; i < vocab_size; ++i) {
        if (logits[i] > logits[best])
            best = i;
    }
    return best;
}

int main(int argc, char **argv) {
    const int ubatch = argc >= 5 ? bounded_int(argv[3], 1, 512) : -1;
    const int steps = argc >= 5 ? bounded_int(argv[4], 0, MAX_GENERATION) : -1;
    const int repeats = argc >= 6 ? bounded_int(argv[5], 1, 5) : 2;
    const bool on_backend = argc >= 7 && strcmp(argv[6], "backend") == 0;
    const bool choose = argc >= 7 && strcmp(argv[6], "choice") == 0;
    const bool save_prefix = argc == 10 && strcmp(argv[8], "save") == 0;
    const bool load_prefix = argc == 10 && strcmp(argv[8], "load") == 0;
    if (argc < 5 || (argc > 8 && !save_prefix && !load_prefix) || ubatch < 0 || steps < 0 || repeats < 0 ||
        (choose && steps != 0) || (argc >= 7 && !on_backend && !choose && strcmp(argv[6], "host") != 0)) {
        fprintf(stderr,
                "Usage: %s MODEL PROMPT_FILE UBATCH STEPS [REPEATS [host|backend|choice [PREFIX_PROMPT [save|load STATE_FILE]]]]\n",
                argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    const double start = seconds();
    ggml_backend_load_all();
    llama_backend_init();
    ggml_backend_dev_t device = ggml_backend_dev_by_name("GPUOpenCL");
    require(device != NULL, "GPUOpenCL unavailable");
    ggml_backend_dev_t devices[] = {device, NULL};
    struct llama_model_params model_params = llama_model_default_params();
    model_params.devices = devices;
    model_params.n_gpu_layers = 99;
    struct llama_model *model = llama_model_load_from_file(argv[1], model_params);
    require(model != NULL, "Could not load model");
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    const int vocab_size = llama_vocab_n_tokens(vocab);
    llama_token options[5];
    if (choose) {
        for (int i = 0; i < 5; ++i) {
            const char digit = '0' + i;
            require(llama_tokenize(vocab, &digit, 1, &options[i], 1, false, false) == 1,
                    "Choice digit needs more than one token");
        }
    }
    llama_token tokens[CONTEXT_TOKENS];
    int count = read_tokens(vocab, argv[2], tokens);
    const char * desired_text = getenv("N20_TEST_PROMPT_TOKENS");
    if (desired_text) {
        int desired = bounded_int(desired_text, 2, 1200);
        require(desired > 0, "Invalid requested prompt length");
        const int source_count = count;
        for (int i = count; i < desired; ++i) tokens[i] = tokens[1 + (i - count) % (source_count - 1)];
        count = desired;
    }
    const char * logpath = getenv("N20_TEST_LOGITS");
    const char * tokenpath = getenv("N20_TEST_TOKENS");
    const bool forced = getenv("N20_TEST_FORCED") != NULL;
    require(logpath && tokenpath && repeats == 1 && !on_backend && !choose, "Logit test configuration invalid");
    FILE * logitfile = fopen(logpath, "wb");
    FILE * tokenfile = fopen(tokenpath, forced ? "rb" : "wb");
    require(logitfile && tokenfile, "Cannot open trial files");
    require(count + steps < CONTEXT_TOKENS, "Prompt plus generation exceeds context");

    struct llama_sampler *sampler = NULL;
    struct llama_sampler_seq_config config = {0};
    struct llama_context_params params = llama_context_default_params();
    params.n_ctx = CONTEXT_TOKENS;
    params.n_batch = CONTEXT_TOKENS;
    params.n_ubatch = (unsigned)ubatch;
    params.n_threads = 2;
    params.n_threads_batch = 2;
    if (on_backend) {
        sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
        require(sampler != NULL, "Could not allocate sampling chain");
        llama_sampler_chain_add(sampler, llama_sampler_init_greedy());
        config.sampler = sampler;
        params.samplers = &config;
        params.n_samplers = 1;
    }
    struct llama_context *context = llama_init_from_model(model, params);
    require(context != NULL, "Could not allocate model context");
    printf("load_and_context_s=%.4f prompt_tokens=%d ubatch=%d sampling=%s\n",
           seconds() - start, count, ubatch, on_backend ? "backend" : choose ? "choice" : "host");

    int prefix = 0;
    size_t snapshot_size = 0;
    uint8_t *snapshot = NULL;
    if (argc >= 8) {
        llama_token reference[CONTEXT_TOKENS];
        const int reference_count = read_tokens(vocab, argv[7], reference);
        while (prefix < count - 1 && prefix < reference_count && tokens[prefix] == reference[prefix])
            ++prefix;
        require(prefix > 0, "Prompts have no shared prefix");
        if (!load_prefix) {
            struct llama_batch batch = llama_batch_get_one(tokens, prefix);
            int8_t skip_logits[CONTEXT_TOKENS] = {0};
            batch.logits = skip_logits;
            const double snapshot_start = seconds();
            require(llama_decode(context, batch) == 0, "Prefix prefill failed");
            llama_synchronize(context);
            if (save_prefix) {
                snapshot_size = llama_state_seq_save_file(context, argv[9], 0, tokens, prefix);
                require(snapshot_size > 0, "Could not save prefix state");
            } else {
                snapshot_size = llama_state_seq_get_size(context, 0);
                require(snapshot_size > 0 && snapshot_size <= 128 * 1024 * 1024, "Unexpected prefix state size");
                snapshot = malloc(snapshot_size);
                require(snapshot != NULL, "Could not allocate prefix state");
                require(llama_state_seq_get_data(context, snapshot, snapshot_size, 0) == snapshot_size,
                        "Could not read prefix state");
            }
            printf("prefix_tokens=%d snapshot_bytes=%zu build_s=%.4f\n",
                   prefix, snapshot_size, seconds() - snapshot_start);
        }
    }

    for (int run = 0; !save_prefix && run < repeats; ++run) {
        llama_memory_clear(llama_get_memory(context), true);
        llama_synchronize(context);
        const double prefill_start = seconds();
        if (load_prefix) {
            llama_token stored[CONTEXT_TOKENS];
            size_t stored_count = 0;
            require(llama_state_seq_load_file(context, argv[9], 0, stored, CONTEXT_TOKENS, &stored_count) > 0,
                    "Could not load prefix state");
            require(stored_count == (size_t)prefix &&
                    memcmp(stored, tokens, stored_count * sizeof(*stored)) == 0,
                    "Snapshot token prefix does not match prompt");
        } else if (snapshot) {
            require(llama_state_seq_set_data(context, snapshot, snapshot_size, 0) == snapshot_size,
                    "Could not restore prefix state");
        }
        const double restored = seconds();
        require(llama_decode(context, llama_batch_get_one(tokens + prefix, count - prefix)) == 0,
                "Prefill failed");
        llama_token generated[MAX_GENERATION + 1];
        generated[0] = greedy(context, vocab_size, on_backend, options, choose ? 5 : 0);
        const float * first_vector = llama_get_logits_ith(context, -1);
        require(first_vector && fwrite(first_vector, sizeof(float), vocab_size, logitfile) == (size_t)vocab_size, "First logits write failed");
        if (forced) require(fread(&generated[0], sizeof(generated[0]), 1, tokenfile) == 1, "Forced token read failed");
        else require(fwrite(&generated[0], sizeof(generated[0]), 1, tokenfile) == 1, "Token write failed");
        uint64_t first_hash = UINT64_C(14695981039346656037);
        if (!on_backend) {
            const float *first_logits = llama_get_logits_ith(context, -1);
            require(first_logits != NULL, "No first logits");
            for (int h = 0; h < vocab_size; ++h) {
                uint32_t bits;
                memcpy(&bits, first_logits + h, sizeof(bits));
                first_hash = (first_hash ^ bits) * UINT64_C(1099511628211);
            }
        }
        const double decode_start = seconds();
        for (int i = 0; i < steps; ++i) {
            require(llama_decode(context, llama_batch_get_one(&generated[i], 1)) == 0, "Decode failed");
            generated[i + 1] = greedy(context, vocab_size, on_backend, NULL, 0);
            const float * vector = llama_get_logits_ith(context, -1);
            require(vector && fwrite(vector, sizeof(float), vocab_size, logitfile) == (size_t)vocab_size, "Logits write failed");
            if (forced) require(fread(&generated[i + 1], sizeof(generated[0]), 1, tokenfile) == 1, "Forced token read failed");
            else require(fwrite(&generated[i + 1], sizeof(generated[0]), 1, tokenfile) == 1, "Token write failed");
        }
        printf("run=%d first_token_s=%.4f decode_s=%.4f token_ids=", run + 1,
               decode_start - prefill_start, seconds() - decode_start);
        for (int i = 0; i <= steps; ++i)
            printf("%s%d", i ? "," : "", generated[i]);
        if (!on_backend && steps == 0) {
            const float *logits = llama_get_logits_ith(context, -1);
            uint64_t hash = UINT64_C(14695981039346656037);
            for (int i = 0; i < vocab_size; ++i) {
                uint32_t bits;
                memcpy(&bits, logits + i, sizeof(bits));
                hash = (hash ^ bits) * UINT64_C(1099511628211);
            }
            printf(" logits_hash=%016" PRIx64, hash);
        }
        if (!on_backend && steps != 0) printf(" first_logits_hash=%016" PRIx64, first_hash);
        if (choose) {
            for (int i = 0; i < 5; ++i) {
                if (options[i] == generated[0])
                    printf(" choice=%d", i);
            }
        }
        if (load_prefix)
            printf(" restore_s=%.4f prefix_tokens=%d", restored - prefill_start, prefix);
        putchar('\n');
    }

    require(fclose(logitfile) == 0 && fclose(tokenfile) == 0, "Trial file close failed");
    free(snapshot);
    llama_free(context);
    if (sampler)
        llama_sampler_free(sampler);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
