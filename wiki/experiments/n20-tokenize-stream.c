/* Tokenize length-prefixed UTF-8 strings using only a GGUF vocabulary. */
#include "llama.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 2) { fputs("usage: n20-tokenize-stream MODEL\n", stderr); return 2; }
    llama_backend_init();
    struct llama_model_params params = llama_model_default_params();
    params.vocab_only = true;
    params.n_gpu_layers = 0;
    struct llama_model *model = llama_model_load_from_file(argv[1], params);
    if (!model) { fputs("Cannot load vocabulary\n", stderr); return 1; }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    uint32_t length;
    for (;;) {
        const size_t got = fread(&length, 1, sizeof(length), stdin);
        if (!got && feof(stdin)) break;
        if (got != sizeof(length)) return 1;
        if (length > 1048576) return 2;
        char *text = malloc((size_t)length + 1);
        if (!text || fread(text, 1, length, stdin) != length) return 1;
        text[length] = 0;
        int32_t needed = llama_tokenize(vocab, text, (int32_t)length, NULL, 0, false, false);
        if (needed == 0) {
            if (fwrite(&needed, 1, sizeof(needed), stdout) != sizeof(needed) || fflush(stdout)) return 1;
            free(text);
            continue;
        }
        if (needed > 0) return 1;
        needed = -needed;
        llama_token *tokens = malloc((size_t)needed * sizeof(*tokens));
        if (!tokens) return 1;
        int32_t count = llama_tokenize(vocab, text, (int32_t)length, tokens, needed, false, false);
        if (count < 0 || fwrite(&count, 1, sizeof(count), stdout) != sizeof(count) ||
            fwrite(tokens, sizeof(*tokens), (size_t)count, stdout) != (size_t)count || fflush(stdout)) return 1;
        free(tokens);
        free(text);
    }
    llama_model_free(model);
    llama_backend_free();
    return ferror(stdin) ? 1 : 0;
}
