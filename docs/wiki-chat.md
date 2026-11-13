# Minecraft Wiki chat on the Nord N20

The bridge has answered real chat messages on the phone using the Adreno
GPU. It is enabled on the live server and can use the configured disk swap,
with an emergency memory and temperature guard. Gameplay trials saw longer tail ticks during
inference; see the [end-to-end trial](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-09-wiki-end-to-end.md).

The phone can run the existing 4B model, but it is slow for interactive chat.
A representative warmed GPU-only request took about 39 seconds, and a
separate experimental GPU–Hexagon configuration took about 27 seconds to
its first token on one 323-token prompt. These are different measurements,
not a direct comparison. The hybrid runtime has not been installed or
validated for production.
Recent kernel tuning has produced small gains or regressions; the available
measurements do not establish an order-of-magnitude speedup. See the
[performance assessment](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-optimization-assessment.md).
The [configuration index](wiki-inference-status.md) links the tested profiles,
benchmarks and their deployment status.

The installed launcher, Python modules and dashboard were updated together for
the queue and status tab. The running bridge may temporarily use the preceding
version of its module until restart so that queued questions are not lost.
Experimental GPU–Hexagon kernels have not been installed as a production bundle.

On vanilla Java 26.3, `@wiki` anywhere in a player message can ask the local
assistant a question, such as `Hey @wiki how many copper ingots make a lightning rod?`.
The bridge follows new player-chat lines in the server log. A background worker
retrieves parsed Java Edition excerpts. It answers recognized ingredient,
fuel, storage capacity, and item property questions directly when the retrieved
evidence supplies a unique answer. Ingredient counts require the full ingredient
name and accept common plural forms. A narrowly phrased question about a powered
hopper pushing items is answered directly only when the relevant locked-hopper
rules are present in the same Wiki section and revision. Other questions go to
the local model.
For explicit comparisons, if the initial retrieval has already reached its
four-passage limit and contains at least 1,500 bytes of evidence, the bridge
also retrieves the two named topics separately and adds distinct passages up
to a bounded prompt size. Other Wiki requests keep their original retrieval
path; this targeted expansion does not guarantee a better answer.
The bridge broadcasts its answer and Wiki section link to the public chat
through the existing server console FIFO. Private messages are not supported.
The model can make factual mistakes
even when it cites a passage. The Wiki cannot see the server's world, inventory,
or player activity.

The same bridge also recognizes these commands at the start of a chat message:

- `@pin <note>` saves the player's current position, dimension, and note.
- `@pin_that <note>` saves the block cell the player is looking toward.
- `@unpin <note>` removes one location whose full note matches, ignoring case
  and repeated whitespace. If several locations have that note, use
  `@unpin <note> | <x> <y> <z> | <dimension>` with values from `@map`.
  Indistinguishable duplicate entries are removed one at a time.
- `@death` privately lists up to five recorded deaths and gives directions.
- `@map` lists all shared saved locations in public chat, sorted alphabetically
  without regard to letter case. `@map <search>` finds every note containing
  the complete search phrase, ignoring case. If no note matches, the local model
  interprets the request. Add `?` at the end to use the model even when the
  phrase matches.
- `@help` lists these commands and `@wiki`.

For each matched location in a search, the bridge also privately tells the
requesting player an approximate direction and distance based on a fresh
position, dimension and facing query immediately before each page of directions.
Directions also give compass and vertical offsets from the player's position.
No direction is given across dimensions. If the player cannot be queried, the
bridge reports that privately. Large results are sent as multiple pages. Plain
`@map` lists locations without directions.
For `@map <search>`, directions also describe the closest other saved landmark
in the same dimension, using east, west, north, south and vertical distances.
When the target is already one of the player's closest landmarks, the bridge
omits the directions from another landmark.
The unfiltered `@map` listing does not include these landmark comparisons.

Death tracking uses Minecraft scoreboard objectives `n20_death` (death count)
and `n20_seen` (last recorded count), and stores per-UUID history in
`world/n20-deaths.json`. After detecting a respawn, the bridge waits at least
one second before privately sending the latest location and fresh directions.
Landmark comparisons for deaths include any saved location in the same
dimension. Polling and the console queue can add to that delay. The server only
retains the latest death location itself: if a player dies multiple times before
the bridge can record it, the intervening locations cannot be recovered.
Death detection and real-player respawn timing have not yet been verified in
an in-game trial.

For example, `@map village` lists every pin whose note contains the word
"village"; `@map village?` asks the model to interpret the request instead.
The whole search phrase must occur in a note for the direct match, so
`@map village well` does not directly match "village with a well".

Locations are shared by all players and stored in `world/n20-pins.json`, so
world backups and restores include them. Marker and help replies are broadcast
to public chat. Pin and map operations are queued in arrival order; a queued
pin captures the player's position when its turn is processed, so remain at the
location until the save confirmation. The limit is 10,000 pins with notes of up
to 160 characters. `@pin` records the player's position when the server
processes the request. If the position or
dimension cannot be read, no pin is saved. Direct `@map` matches can answer
while the model is busy or unavailable; requests needing the model use the Wiki
model queue and resource limits. The model sees notes and local identifiers,
not coordinates; large maps are split into bounded chunks and every chunk is
searched. The bridge accumulates the results and looks up coordinates itself.
Many chunks can take a long time, and a failed chunk fails the search rather
than presenting incomplete results. Matching can select the wrong note.
Its prompt uses normal worker prompt caching, not a precomputed offline KV
cache. The bridge must be enabled for these commands to work.

`@pin_that` captures the player's eye position, facing and dimension as soon as
the bridge processes the chat request. The vanilla chat log does not include
the player's position or facing at the exact instant of the message, so a very
quick movement before that capture can still affect the result. Once the
server says the view was captured, the player may move or turn. The scan uses
that fixed snapshot even if other pin requests are queued.

The ray traverses each block cell it crosses, up to 512 blocks, and saves the
first cell that is not air, clear glass, or a clear glass pane. It targets tinted
glass, stained glass, and stained glass panes. The traversal treats each cell as fully occupied, so
water, plants and partial blocks such as slabs may be targeted even where the
crosshair would pass through their shape. It does not target entities. If a cell
is unavailable because its chunk is not loaded, the scan stops without saving;
it does not force-load chunks or guess past the gap.

The bridge manages scans asynchronously in batches of at most 12 cells,
spaced at least 20 ms apart, and rotates unfinished scans through the queue so
multiple players can pin concurrently. Up to 128 marker operations can wait in
the queue. Minecraft still executes each block check on its server thread; the
batching limits each burst but cannot guarantee there will be no gameplay impact.
Long scans take longer, particularly when multiple players are pinning. If the
capture or scan fails, no pin is saved.
The capture briefly creates an effect-free area cloud at the player's eyes and
removes it; if interrupted, the cloud expires after ten seconds while loaded.

The bridge immediately acknowledges a valid question in public chat. It queues
additional questions in arrival order, tells their senders they are queued,
and processes one at a time, with at least 30 seconds between request starts.
The queue is held in memory and is discarded if the bridge or server restarts.
If the model is unavailable or resources are low, questions remain queued
until it is ready and the resource guard permits processing. The Wiki tab on
the phone shows model health, resource limits, the active question, its current
stage and elapsed time, and the first 128 queued questions; additional questions
stay in the queue. During streaming it also shows received character count;
it does not claim a completion percentage when none is available.
The Wiki History screen shows up to 50 completed public questions and replies,
newest first, with paged summaries and a View button for the complete response.
New history is retained across bridge restarts in a mode-600 file
under the Minecraft-owned `/var/lib/n20-wiki/history` directory; previous
conversations were not retroactively imported. The phone's Server → Log view
has Newer and Older controls and displays the current and total page numbers.
The total may briefly show an ellipsis while a large log is counted.
On a cache miss, the retriever may send search terms to Minecraft
Wiki and cache the parsed articles. An offline invocation of the bridge
searches cached pages only. The bridge caches up to 64 sourced replies in
memory. Reusing a reply requires the same question and the same retrieved
evidence, including the page revisions. The bridge pauses lookups at 40 °C
battery temperature or above. Without at least 512 MiB of free swap, the
bridge requires 980 MiB of available RAM; with swap available it requires
208 MiB. The launcher normally waits for 3.5 GiB of available RAM, but permits
startup with at least 336 MiB of available RAM and 2 GiB of free swap. It
stops the worker if RAM falls below 208 MiB while at least 512 MiB of swap is
free, or below 980 MiB otherwise, or if the battery reaches 40 °C. The bridge
remains available to acknowledge and queue questions while the worker is off.
Swap can cause substantial storage traffic and gameplay stalls.

## GPU worker setup

This launcher is specific to the C.24 vendor kernel and matching Android
userspace libraries. It runs Android ARM64 llama.cpp b12110 under the
existing Bionic linker and uses the C.24 Adreno OpenCL 2.0 driver.
The tested build used Android NDK r30, API 30, and
`-march=armv8.2-a+fp16+dotprod` for C and C++. It set
`GGML_CPU_ARM_ARCH=armv8.2-a+fp16+dotprod` and `GGML_OPENCL_TARGET_VERSION=200`
and enabled `GGML_OPENCL`, `GGML_OPENCL_EMBED_KERNELS` and
`GGML_OPENCL_USE_ADRENO_KERNELS`. It disabled `GGML_NATIVE`, `GGML_HEXAGON`,
`GGML_RPC`, `GGML_OPENMP`, `LLAMA_CURL` and `LLAMA_OPENSSL`.
The compatibility shim supplies the signed packed
dot-product intrinsic that the C.24 OpenCL compiler lacks. Build the shim
as a shared object with the Android ARM64 toolchain.

A non-deploying helper can prepare the tuned, activation-texture,
Gated DeltaNet and disk-prefix patches together from a Git checkout of the
exact b12110 commit. It archives that commit into a new output directory,
applies the patches there, builds Android ARM64 binaries, stages the tested
kernel sources, and records hashes. It requires Python 3.12 or later, Git,
CMake, Ninja, the Android NDK, OpenCL headers and an Android ARM64 OpenCL link
library. The output must be a new directory outside both source checkouts.
For example, on a host with these prerequisites:

```sh
python3 tools/build-wiki-gpu.py --source "$LLAMA_SRC" --ndk "$ANDROID_NDK" \
    --opencl-include "$OPENCL_HEADERS" --opencl-library "$OPENCL_LINK_LIBRARY" \
    --output "$WIKI_BUILD_OUTPUT"
```

The resulting `bundle/` is partial: it does not contain the model, the
compatibility shim, C.24 drivers or firmware, the retriever, or the launcher.
`manifest.json` records the upstream commit and hashes of patches and staged
artifacts, but is not a complete fingerprint of toolchains and device runtime.
The helper does not modify the phone or install files. Building successfully
is not a gameplay or answer-quality validation. See the [scratch build check](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-gpu-build.md).

```sh
aarch64-linux-android30-clang -O2 -shared -fPIC \
    wiki/gpu-worker/n20-opencl-compat.c -o cl-compat.so -ldl
```

Use the matching C.24 OTA's `vendor/firmware` files `a630_sqe.fw`,
`a615_zap.mdt`, and `a615_zap.b00` through `a615_zap.b02` for the KGSL
driver. The launcher creates restricted KGSL and ION character devices from
their registered sysfs major and minor numbers. Reserve numeric UID and GID
61124 for this worker. Use `n20-wiki-start --devices-only` as root to prepare
the two device nodes for an experiment without starting the chat bridge.
Install the files into root-controlled paths.

| Path | Contents |
| --- | --- |
| `/opt/n20-wiki/n20-wiki` | ARM64 Rust retriever from [the wiki build](../wiki/README.md) |
| `/opt/n20-wiki/opencl/bin/` | Android ARM64 `llama-server` and its shared libraries |
| `/opt/n20-wiki/opencl/{vendor,system}/` | Matching C.24 OpenCL driver and its runtime dependencies, with notices |
| `/opt/n20-wiki/opencl/cl-compat.so` | Packed dot-product compatibility shim |
| `/usr/local/lib/n20-radio/android/linker64` | Existing C.24 Bionic linker, beside its libraries |
| `/opt/n20-wiki/model.gguf` | Verified GGUF checkpoint |
| `/usr/local/lib/n20/` | `wiki_chat.py`, `wiki_answer.py`, `wiki_fast.py`, and `wiki_prefix.py` from the runtime tree |
| `/usr/local/sbin/n20-wiki-start` | Launcher from the runtime tree, mode 0755 |
| `/var/lib/n20-wiki/cache` | Parsed articles and SQLite index, mode 0700 owned by `minecraft` |
| `/var/lib/n20-wiki/shaders/opencl` | OpenCL kernel cache, mode 0700 owned by UID and GID 61124 |

The model serves one client slot over a local Unix socket with a 4,096-token
context. A [workstation slot-state audit](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-11-wiki-slot-state-size.md)
estimated about 32 KiB of KV state per
token for this model; increasing the context from 1,536 tokens may therefore
require roughly 80 MiB more, excluding other runtime allocations. The higher
memory guards provide additional reserve, but the actual phone cost and gameplay
impact still need measurement. Its CPU threads
are restricted to efficiency cores and run at reduced priority. It requests
the OpenCL GPU explicitly and releases the model from memory after 90
seconds of inactivity. A request that wakes the model pays the reload cost.
While awake, the single slot can reuse the unchanged portion of the prompt.
The launcher passes `--cache-ram 0` to skip saving older model states in a
separate RAM cache. The chat bridge still reuses sourced replies after
checking the current evidence. A model request for an older question that
misses the reply cache may need more prefill. See the
[prompt cache comparison](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-prompt-cache.md).
The bridge lets the worker generate through its normal end-of-generation token,
so a blank line before the answer does not cut the reply short. It reads model
output as it arrives and extracts the answer and its cited passage
after the worker reports a normal stop. Ordinary Wiki prompts allow up to 75
seconds for the first text, 15 seconds between text updates, and 105 seconds
total for the model request. Longer prompts get additional first-text and total
time; requests allowing longer output also get additional total time. Each
retrieval step can take up to 20 seconds.
When the model deadline expires, the bridge closes the connection and sends
the player an unavailable message. See the [streaming deadline trial](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-stream-deadline.md)
for the phone comparison. The model worker has its own UID. The bridge runs
as `minecraft` to use the game-owned console FIFO.

The supervisor starts the worker when `/etc/n20-wiki-enabled` exists. To
enable a tested installation without restarting Minecraft, create the marker
and start the launcher in the background under a separate session. Use its
log to verify that llama.cpp selected the Adreno device and offloaded the
model. Removing the marker shuts down the bridge and the model on the next
launcher check.

For the optional disk-backed system prefix, build llama-server with both
`wiki/gpu-worker/llama-b12110-n20-server.patch` and
`wiki/gpu-worker/llama-b12110-n20-disk-prefix.patch`, in that order. Install
the patched server and `wiki_prefix.py`, create the root-controlled regular
file `/etc/n20-wiki-disk-prefix`, and restart the Wiki launcher. The launcher
fingerprints the protected model, runtime, firmware, boot ID, and exact worker
options before enabling the cache. It stores one private snapshot under
`/var/lib/n20-wiki/prefix` and uses it only during the same boot. If validation
fails, it logs the reason and uses ordinary prefill. The [phone trial](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-disk-prefix.md)
includes launch-time fingerprinting and compares replies and logits.

```sh
install -m 644 /dev/null /etc/n20-wiki-enabled
/usr/local/bin/busybox setsid /usr/local/sbin/n20-wiki-start \
    >> /var/log/n20-wiki.log 2>&1 </dev/null &
```

## Experimental Adreno kernels

The [inference tuning trial](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-09-wiki-inference-tuning.md)
compares custom Q2_0 prefill and decode kernels, a direct recurrent-state
copy, and a reusable prompt-prefix snapshot on this phone.

The launcher has an optional OpenCL profile for the tested Bonsai 2 4B
group-64 Q2_0 checkpoint. Build llama-server with the OpenCL patch and the
two server patches described above. Stop the Wiki launcher before replacing
the runtime or kernel files. Stage the built OpenCL library beside
llama-server and install the four kernel files below in a root-owned
directory with mode 0755. Run these commands from a copy of the checkout on
the phone as root.

```sh
install -d -m 755 /opt/n20-wiki/kernels
install -m 644 wiki/gpu-worker/n20-q2-decode.cl wiki/gpu-worker/n20-q2-pack.cl \
    wiki/gpu-worker/n20-fwht-radix.cl /opt/n20-wiki/kernels/
prefill=$(mktemp)
{ printf '#define N20_PARTIAL 64\n'; cat wiki/gpu-worker/n20-q2-prefill.cl; } > "$prefill"
install -m 644 "$prefill" /opt/n20-wiki/kernels/n20-q2-prefill-64.cl
rm "$prefill"
install -m 644 /dev/null /etc/n20-wiki-opencl-tuned
```

Restart the Wiki launcher to apply the marker. The profile enables the
64-column prefill kernel with FP32 outer accumulation, the four-lane tail,
the tested decode shapes and wide vocabulary tile, direct weight packing
and recurrent-state copy, and the width-512 Hadamard transform. When the
disk prefix is also enabled, single-turn Wiki batching skips two tail
checkpoints and uses a separate snapshot in `/var/lib/n20-wiki/prefix-tuned`.
The snapshot identity includes the source contents and the worker options.
Invalid or missing inputs select stock OpenCL kernels. If a tuned worker
exits, the launcher retries once with stock kernels and the stock prefix
cache. Remove `/etc/n20-wiki-opencl-tuned` and restart the launcher to
select stock kernels on subsequent starts. See the
[isolated launcher comparison](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-tuned-launcher.md)
for reply, startup and prefix restore measurements.

### Activation texture profile

Apply `wiki/gpu-worker/llama-b12110-n20-activation-image.patch` after
the N20 OpenCL patch and rebuild the Android ARM64 OpenCL backend with
`GGML_OPENCL_TARGET_VERSION=200`. With the tuned profile staged and the
Wiki launcher stopped, place the rebuilt library beside `llama-server`.
Install the texture source and add the separate marker as root.

```sh
install -m 644 wiki/gpu-worker/n20-q2-activation-image-prefill.cl /opt/n20-wiki/kernels/
install -m 644 /dev/null /etc/n20-wiki-opencl-texture
```

Restart the Wiki launcher. It accepts this profile only when tuned
validation passes and `model.gguf` matches the tested Bonsai 2 4B
group-64 Q2_0 checkpoint. The worker checks that its device and in-order
queue support the required RGBA FP16 image, checks kernel workgroup
capacity, and reuses one 64 × 9,216 texture with a 4.5 MiB pixel payload.
It sets a 256-token microbatch and uses the texture for the tested
feed-forward dimensions with at least 16 tokens. Other projections use
the tuned image-buffer kernels.

Validation failures select tuned kernels. If texture setup fails or the
texture worker exits, the launcher starts the tuned worker, then retries
with stock kernels if the tuned worker exits. If a texture argument,
copy, or launch call returns an immediate OpenCL error during inference,
the worker disables the texture for that context and completes that
projection with the tuned image buffer. The in-order queue keeps the
copy and GEMM ordered without a per-projection wait.

The texture profile has a private shader cache in
`/var/lib/n20-wiki/shaders-texture`. With the disk prefix enabled it
uses a separate snapshot under `/var/lib/n20-wiki/prefix-texture`, keyed
to both prefill sources, the backend, checkpoint, and worker options. Texture
validation and prefix fingerprinting share a single full checkpoint hash;
the helper checks that the model file identity remains unchanged between
validation and fingerprinting.
Remove `/etc/n20-wiki-opencl-texture` and restart the launcher to select
the tuned profile. See the [initial activation texture trial](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-activation-texture.md)
for kernel and model comparisons and the [launcher trial](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-texture-launcher.md)
for complete replies and fallback checks.

### Flash Attention shader cache

Apply the optional Flash Attention cache patch after the selected N20
OpenCL patches and rebuild the Android ARM64 backend.

```sh
git -C "$LLAMA_SRC" apply "$MINOS/wiki/gpu-worker/llama-b12110-n20-fa-cache.patch"
cmake --build "$LLAMA_BUILD" --target ggml-opencl
```

The worker adds lazy Flash Attention programs to its existing private
shader cache. It builds missing or unusable entries from source with the
existing retry and fallback behavior. Once the entries are populated, the
next fresh worker process can reuse them. On the texture profile this cut
process launch to health from 18.42 to 7.47 seconds in a paired run with
identical cited replies. [Phone trial](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-fa-cache.md)

## Cache maintenance

Run these commands on the phone as root. `--update` checks revisions for
articles due for revalidation, fetches changed pages, and updates the index.
Run it during an idle period. `--index` indexes new or changed parsed files
without contacting the Wiki. Failed refreshes leave the previous parsed copy
available.

```sh
runuser -u minecraft -- nice -n 10 /opt/n20-wiki/n20-wiki \
    --cache /var/lib/n20-wiki/cache --update
runuser -u minecraft -- nice -n 10 /opt/n20-wiki/n20-wiki \
    --cache /var/lib/n20-wiki/cache --index
```
