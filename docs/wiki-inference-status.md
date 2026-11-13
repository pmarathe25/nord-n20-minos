# Nord N20 Wiki inference: tested configurations

This page is an index to the phone experiments for the current Bonsai 2 4B
Q2_0 checkpoint. It is not a deployment recipe or a claim that the listed
experimental files are installed. The Wiki bridge is disabled, and a
2026-11-10 comparison found that the installed launcher and Python modules
differ from the checkout. See [Wiki chat](wiki-chat.md) for the current
configuration, safety checks, build inputs, and limitations.

## Configurations and evidence

| Configuration | What was measured | Status |
| --- | --- | --- |
| Tuned GPU kernels | In one restored-prefix launcher trial, a 34-token model reply took 39.82 seconds versus 56.27 seconds with the stock profile. This comparison did not retain both complete bridge timings. | Scratch launcher, [measurement and fallbacks](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-tuned-launcher.md). |
| GPU activation texture and disk system prefix | In one launcher trial, a restored 106-token prefix led to a 38.45-second complete bridge reply. The empty-prefix reply was 49.41 seconds. | Scratch launcher, [measurement and failure injection](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-texture-launcher.md). |
| Texture plus two-column Gated DeltaNet | A restored-prefix, simulated chat-to-reply comparison took 41.399 seconds versus 43.403 seconds for four columns on one question. Both supplied the same answer and first-token logit hash. | Checkout opt-in profile, [comparison and fallback](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-gdn-launcher.md). The backend and marker were not deployed in that trial. |
| GPU plus Hexagon | A matched eight-question model-request comparison had a median paired improvement of 1.114 seconds over GPU-only; both scored 5 of 8. It excluded retrieval, startup, game chat, and gameplay. | Scratch approximate runtime, [comparison](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-hybrid-microbatch.md). It is not a supported launcher profile. |
| GPU plus Hexagon and fixed-prefix cache | Two different questions reused a 106-token prefix; first-token inference fell from 27.77 to 19.00 seconds and 23.19 to 14.41 seconds, excluding approximately 6.6 seconds of loading. Fresh and restored first-token logits matched within each question. | Scratch-only [identity trial](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-hybrid-prefix-identity.md); no full-answer, gameplay, or production cache-identity validation. |
| GPU plus Hexagon down projection | With corrected no-thinking prompt framing, two full model-worker answers saved about 0.13–0.49 seconds to first token against warm controls; both arms answered one of the two questions incorrectly. | Scratch-only [model trial](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-hybrid-down-model.md); additional Q8 arithmetic changes logits, and no actual game-chat or gameplay comparison was made. |
| Down projection, streaming Wiki comparison | Eight matched model requests had identical replies and 4/8 passed; the candidate's median paired request time was 0.05 seconds slower in this single-order run. | Scratch-only [streaming comparison](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-hybrid-down-streaming.md); it forced model inference for two questions that have an exact-answer route. |
| Short-batch block-major Hexagon input | The isolated 16-token mixed projection improved by about 17%; a 323-token full-model prefill did not improve. | Not selected, [projection and model results](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-block-major-screen.md). |
| Two-stage GPU decode reduction | One isolated projection was slightly faster, but a bracketed full-model test did not establish a decode improvement and first-token logits changed. | Not selected, [assessment](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-optimization-assessment.md). |
| Global split GPU decode reduction | Two-dispatch FFN decode reduced the median of eight paired streaming-model requests by 0.31 seconds in a single-order trial; both arms scored 4/8 with identical replies. | Scratch-only [projection and model trial](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-decode-global-split.md); no active-player gameplay or broad answer-quality validation. |
| Global split vocabulary projection | The isolated vocabulary projection was about 27% faster; adding it to the FFN-optimized path did not establish a streaming-request gain in eight matched questions. | Scratch-only [vocabulary trial](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-vocab-global-split.md); both workers scored 4/8 with identical replies. |

These rows describe different trials and must not be combined as a single
speed comparison. Inputs, cache state, measured intervals, worker builds, and
use of the Hexagon differ. Full model latency includes prefill and generation;
actual chat can also include retrieval, model startup and message delivery.

## Gameplay and accuracy limits

A tuned GPU profile was tested with a disposable server and one client. Three
model replies arrived in 32.20–45.70 seconds; a fourth timed out. The movement
and stationary samples do not establish no-noticeable-lag behavior for normal
gameplay. See the [gameplay trial](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-gameplay.md) and
[texture-profile gameplay trial](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-texture-gameplay.md).
The model can still give incorrect answers and citations. The hybrid's small
quality comparison is not sufficient to establish general equivalence.

## Rebuilding and evaluating

The checked-in [launcher and build notes](wiki-chat.md) list the b12110
patches, runtime paths, optional profile markers, prefix identity and fallback
behavior. The notes describe the development checkout; they do not mean that
all artifacts are present on the phone. Do not mix a shader with a backend or
launch geometry it was not built and tested for. In particular, the
experimental subgroup decode source assumes its documented fixed geometry,
and the block-major DSP path uses a separate module and packer to avoid
mixing activation layouts.

Do not compare a microbenchmark directly with chat latency. For a candidate,
first check numerical behavior, then compare matched complete requests, answer
and citation quality, memory and thermal behavior, and active-player tick
tails. Keep the existing server, world, credentials, firmware and recovery
material intact. Do not flash device partitions for benchmarking.

The broader [optimization assessment](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-optimization-assessment.md)
records the current bottleneck analysis and untested directions. Detailed
kernel and scheduling investigations are preserved in Git history; scratch
logs, model weights and private prompts do not belong in the repository.

A later [one-token attention experiment](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-phase-attention.md)
retained the Flash Attention cache layout and used the ordinary attention graph
for eligible single-token work. Two oppositely ordered eight-request streaming
comparisons saved a median 0.4 and 0.75 seconds per pair, with identical response
text in each comparison. Teacher-forced tests on four contexts retained the same
top token across 17 steps, including a one-token prefill boundary. It remains
an uninstalled experiment pending broader numerical and gameplay testing. An
instrumented run confirmed the relevant operations reached the GPU backend. It
does not solve the overall latency.
The global-split [FFN](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-decode-global-split.md)
and [vocabulary](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-10-wiki-vocab-global-split.md) results
are also experimental rather than installed production improvements.
