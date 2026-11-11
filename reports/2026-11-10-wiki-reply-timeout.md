# Wiki reply stopping and format trial, November 10, 2026

The [gameplay trial](2026-11-10-wiki-gameplay.md) timed out after 75.13
seconds with 124 generated tokens. The bridge waits for a complete
non-streamed response and accepts it when the model reports a normal stop.
It asks for one support line and one answer line, with a blank-line stop
and a 140-token generation cap.

In the desktop screen, none of 132 baseline requests yielded a complete
answer line followed by a newline before the normal stop. The format
changes that shortened replies also lost correct answers or details.

## Desktop screen

The desktop ran llama.cpp b12110, Bonsai 2 4B Q2_0 on Vulkan0, six CPU
threads, one slot, and a 2,048-token context. Each mode used temperature
zero, thinking disabled, a 140-token output cap, and no prompt reuse.
The installed Rust retriever ran offline against a temporary copy of the
phone's parsed cache. All modes received the same ranked excerpts for
each question. Request order was shuffled with a fixed seed within each
screen. All replies were collected through the streaming API to observe
when complete lines appeared. Desktop requests overlapped other work,
so the comparison uses generation counts.

The 104 repository questions included 84 factual questions and 20
private-state questions. Twelve additional recipe phrasings covered
lightning rods, hoppers, observers, bookshelves, shields, and rockets.
Factual passes required the expected answer patterns and a link to an
expected article after bridge rendering. The cited-evidence score also
required the expected evidence patterns in the cited passage. The eight
exact-route answers were scored as the bridge would show them. Their
model requests still contributed to the token distributions.

| Mode | Repository factual | Cited evidence | Private abstentions | New recipe phrasings | Output tokens, median / p95 / max |
| --- | ---: | ---: | ---: | ---: | ---: |
| Existing grounded prompt | 65/84 | 52/84 | 20/20 | 9/12 | 34 / 47 / 56 |
| Support in at most eight words | 63/84 | 51/84 | 20/20 | 8/12 | 22 / 33 / 38 |
| Template with example support and answer lines | 60/84 | 51/84 | 20/20 | 9/12 | 27 / 45 / 58 |
| Key words or numbers only in support | 24/84 | 23/84 | 20/20 | 3/12 | 11 / 25 / 39 |
| Requested terminal marker | 64/84 | 52/84 | 17/20 | 8/12 | 37 / 57 / 73 |
| Requested terminal marker with grammar | 64/84 | 52/84 | 17/20 | 8/12 | 37 / 56 / 73 |
| DRY repetition penalty | 65/84 | 52/84 | 20/20 | 9/12 | 34 / 47 / 56 |

The eight-word instruction saved 12 tokens at the median. It switched
the Silk Touch ender chest answer from the chest itself to eight obsidian
blocks, and dropped the copper ingot count from one lightning rod recipe
reply. The key-words-only variant frequently omitted citation brackets.
The terminal-marker variants sometimes returned the abstention sentence
without its final period. In three private-state cases the renderer
treated that text as a sourced answer. DRY used a 0.8 multiplier and an
allowed repeat length of four tokens. It changed eight raw texts without
changing the pass counts or output-token distribution at the reported
percentiles.

## Longer replies and streaming

Sixteen separate prompts asked for full recipes, comparisons, or several
details at once. The exact gameplay wording was unavailable. Baseline
generation ranged from 28 to 100 tokens with a 41.5-token median. The
eight-word support variant ranged from 13 to 44 tokens with a 32.5-token
median. Its 36-token reply to the quartz-block observer question said to
use Nether Quartz and omitted the full recipe and the requested comparator
comparison. The baseline generated 100 tokens for that question. On the
rocket comparison, the variant cut generation from 78 to 39 tokens and
omitted the requested effect of adding gunpowder. DRY reproduced all
16 baseline texts and token counts.

All 132 baseline streams ended normally, with one newline in each
returned text. A client waiting for a newline after the answer could
therefore release none of these replies earlier. Stopping inside the
answer line would require a different completion rule, and the longer
replies placed requested details late on that line.

The Android ARM64 tuned worker was rebuilt in workstation scratch.
The formats with a useful decode reduction lost answer accuracy in
the desktop comparison. Phone work was limited to offline retrieval
against temporary cache copies. The gameplay timeout remains
unreplicated.
