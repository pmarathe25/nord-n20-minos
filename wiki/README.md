# Minecraft Java Wiki retrieval

`n20-wiki` turns a player question into a short set of sourced passages. It
uses a local SQLite full-text index over parsed Java Edition articles. Give the
worker a cache directory owned by its service account and run it off the
Minecraft server's tick thread.

```sh
n20-wiki --cache /path/to/wiki-cache --question 'How many ingots make a lightning rod?'
n20-wiki --cache /path/to/wiki-cache --offline --question 'How many ingots make a lightning rod?'
printf '%s\n' '"How many ingots make a lightning rod?"' |
  n20-wiki --cache /path/to/wiki-cache --offline --batch
```

Question mode returns JSON with up to four excerpts under `passages`. Each
excerpt includes the canonical title, revision, section, anchor, and text. The
response also reports `fetched` and `warnings`. A batch worker takes one JSON
string per line and returns one response per line. It reports a malformed
request with an `error` field and can continue reading.

On a lookup miss, online mode sends search terms to the Minecraft Wiki API over
HTTPS. It tries titles and redirects, then falls back to text search when needed.
It can also check for a missing article when a cached title only matches part of
the question. It remembers successful discovery checks for a day and fetches at
most two articles per question. Only the cleaned, parsed article
enters the cache. Offline mode searches pages already cached, including those
added by a maintenance process while the batch worker is running.

## Cache and index maintenance

```sh
n20-wiki --cache /path/to/wiki-cache --index
n20-wiki --cache /path/to/wiki-cache --update
n20-wiki --cache /path/to/wiki-cache --update --max-age-hours 0
```

`--index` can run as a background job. It picks up existing parsed files and
indexes only new or changed files. A new worker also synchronizes the index at
startup. Parsed JSON is the source of truth. To rebuild a damaged index, stop
workers using that directory, remove the derived SQLite index, then run
`--index` again.

`--update` checks revisions in batches for pages last fetched or checked at least
seven days ago. `--max-age-hours 0` checks every cached page. The command
fetches and indexes changed pages and reports unchanged, refreshed, and failed
checks as JSON. A failed refresh preserves the last usable cache entry and the
command exits with a failure status if any checks failed. Use `--refresh TITLE`
to force one page to be fetched and reparsed even if its revision is unchanged.

## Inspect an article

```sh
n20-wiki --cache /path/to/wiki-cache Hopper
n20-wiki --cache /path/to/wiki-cache --offline --list Hopper
n20-wiki --cache /path/to/wiki-cache --section Sounds Hopper
n20-wiki --cache /path/to/wiki-cache --refresh Hopper
```

The default output is the cleaned gameplay view. A section request includes
that section and its descendants. History and Sounds are available on request.
A crafting grid with unambiguous item labels contributes ingredient counts as
plain text. For a grid with animated alternatives, the parser reports only
counts that the alternatives cannot change. Refresh older parsed copies of
affected pages to pick up these counts. A missing anchor returns an empty
section list. The parser uses
the article's edition annotations. Exact game-release filtering is a
separate step.

## Desktop model benchmark

Build the Rust executable with Cargo and start a local llama.cpp
`llama-server` with a compatible GGUF checkpoint. The Python harness calls the
same Rust batch interface used for retrieval. The expected article titles in the
case file are scoring labels. The retriever and the model receive the player
question without those labels.

```sh
cargo build --manifest-path wiki/Cargo.toml --locked --release
llama-server --model /path/to/Ternary-Bonsai-2-4B-Q2_0.gguf --host 127.0.0.1 --port 8080
python3 wiki/benchmark.py --wiki wiki/target/release/n20-wiki \
  --cache /path/to/wiki-cache --output answers.jsonl
```

The harness uses an offline cache by default. Preload the articles for a warm
test, or use `--online` and a new cache for a cold test. It reports answer
accuracy, evidence matches, and separate retrieval and generation timings.
Review the answers alongside the pattern-based scores.

Use `--cases wiki/benchmark-fresh.json` for the follow-up question set.
Use `--cases wiki/benchmark-recipes.json` to check recipe counts.
Use `--answer-style grounded` to test the single-call support-first prompt.
The harness records the supporting line for review and scores the player-facing
answer separately.

## Build and test

For a static ARM64 Linux build, install Zig and `cargo-zigbuild`, and add the
`aarch64-unknown-linux-musl` Rust target.

```sh
cargo test --manifest-path wiki/Cargo.toml --locked
cargo zigbuild --manifest-path wiki/Cargo.toml --locked --release --target aarch64-unknown-linux-musl
```

The cross-build outputs `wiki/target/aarch64-unknown-linux-musl/release/n20-wiki`
unless `CARGO_TARGET_DIR` or `--target-dir` is set.
