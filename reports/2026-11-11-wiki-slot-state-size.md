# Wiki slot-state size audit, November 11, 2026

The workstation ran the same Bonsai 2 4B Q2_0 GGUF and llama.cpp b12110
server binary used in the earlier page-prefix trial. The server used Vulkan,
one slot, a 4,096-token context and otherwise default cache types. This was an
isolated server; the phone and production Wiki bridge were not changed.

The probe sent controlled token arrays to `/completion` with `n_predict=0` and
`cache_prompt=false`, then saved the slot. It repeated the procedure with
default context checkpoints and with `--ctx-checkpoints 0`:

| Tokens | Default file bytes | Checkpoints disabled bytes |
| ---: | ---: | ---: |
| 1 | 52,724,580 | 52,724,580 |
| 32 | 106,432,856 | 53,741,256 |
| 128 | 109,581,272 | 56,889,672 |
| 256 | 113,779,160 | 61,087,560 |
| 512 | 122,174,936 | 69,483,336 |
| 1,024 | 191,658,076 | 86,274,888 |
| 2,048 | 225,241,180 | 119,857,992 |

The server log explicitly reported appending one 50.251 MiB context checkpoint
for the middle lengths and two totaling 100.501 MiB for 1,024 and 2,048 tokens.
These are extra recurrent-state checkpoints for restoring earlier points in a
prompt, not additional input text. The build's server code appends checkpoints
to the slot file. Without them, the measured file is approximately 52.7 MB plus
32,796 bytes per token. The increment is consistent with eight attention layers,
four KV heads, 256 values per head, both K and V, at two bytes each (32,768
bytes per token), plus 28 bytes of per-token serialization metadata. This is an
inference from the file sizes and architecture, not a byte-by-byte file audit.
The large initial state is consistent with the recurrent layers; the one-token
probe also includes the first token's attention state and metadata.

With checkpoints disabled, an independent synthetic prefix of 482 tokens was
saved and restored. Continuing with the same prefix reused all 482 tokens,
processed ten new prompt tokens, and produced exactly the same greedy output as
the cold run. After changing one token at position 100, the restored run reused
zero tokens and evaluated all 492 prompt tokens. This is one trial, not a
guarantee across templates, models, prompt boundaries, or runtimes. Removing
checkpoints sacrifices rollback to earlier matching points; it needs further
testing before changing production or the page cache generator defaults.

The earlier 118–193 MB page files included these appended checkpoints, so the
prior per-token estimate derived from those files was misleading. Page snapshots
would still contain roughly one 52.7 MB recurrent state and the token-dependent
attention state in this tested configuration. Lowering cache precision, sharing
data, or using a different state format may reduce space, but were not tested.

For a non-recurrent comparator, Qwen3-4B-Instruct-2507 has 36 attention layers,
eight KV heads and head dimension 128. Its theoretical FP16 K/V data is
36 × 8 × 128 × 2 (K and V) × 2 bytes = 147,456 bytes per token before metadata,
or about 147 MB per 1,000 tokens; actual runtime state size can differ. Thus
removing recurrence does not automatically reduce the total at longer lengths.
No comparison model was downloaded or run as part of this audit.
