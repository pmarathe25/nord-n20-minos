# Nord N20 Bonsai decode host work, November 10, 2026

The Nord N20 ran the installed llama.cpp b12110 OpenCL backend and
Bonsai 2 4B Q2_0. The test used a scratch model process at nice 15,
a private shader cache and a shared lock for GPU access. Minecraft
was online with zero players at the status check. The model used a
1,536-token context, a 256-token microbatch limit and two host threads.

The benchmark used a 67-token rendered Minecraft torch question
with an excerpt stating the torch's light level. It built a
66-token in-memory prefix snapshot and restored that state before
each run. The timer starts after selecting the first token and
includes 16 forward passes with greedy sampling. Each run produced
17 token IDs including the first token.

## CPU affinity

The process began on efficiency cores. Before each repeat, it set
the affinity of all three observed process threads by enumerating
`/proc/self/task`. Model thread counts and priority were fixed. The
masks ran in efficiency, performance,
efficiency, performance, performance, efficiency, performance,
efficiency order with host greedy sampling.

| CPU affinity | Decode seconds by run | Median seconds | Tokens/s |
| --- | --- | ---: | ---: |
| 0–2, efficiency cores | 8.0328, 8.0352, 8.0346, 8.0352 | 8.0349 | 1.99 |
| 6–7, performance cores | 7.6292, 7.6277, 7.6258, 7.6270 | 7.6274 | 2.10 |

All eight runs produced the same token IDs. Performance cores cut
median decode time by 5.1%. The Java process was eligible to run
on all eight cores, so the performance mask places the model on
cores that can also serve Minecraft. This test measured neither
active-player tick latency nor battery use.

## GPU greedy readback

The [earlier argmax trial](2026-11-10-wiki-greedy-argmax.md) changed the
reduction kernel while keeping the standard sampling graph and its
full vocabulary readback. This follow-up kept the stock reduction
and varied the graph outputs. It ran on CPUs 0–2.

A terminal wrapper around the stock greedy sampler left the selected
token tensor in the graph outputs and cleared the sampled-logits
pointer. The harness checked that stock GPU greedy exposed 248,320
sampled logits on every decode step and that the wrapped variants
exposed zero. A second wrapper also took the logits directly from
the source of the padding operation when it found a contiguous
single-row source and a view starting at offset zero. The normal
path remains available for other shapes.

The test ran each mode twice in one process, in table order and
then reverse order, with a fresh sampler chain for each GPU run.

| Sampling | Decode seconds by run | Median seconds |
| --- | --- | ---: |
| Host greedy | 8.0865, 8.0759 | 8.0812 |
| Stock GPU greedy | 8.9986, 8.9979 | 8.9983 |
| GPU greedy, token output only | 8.9867, 8.9865 | 8.9866 |
| GPU greedy, token output only and no padding | 8.7353, 8.7460 | 8.7407 |

All eight runs produced the same token IDs as the affinity test.
Omitting the sampled logits changed median decode time by 0.13%.
Omitting both the padding and sampled logits cut median time by
2.9% versus stock GPU sampling, leaving it 8.2% slower than host
greedy. The host and GPU reductions can break ties differently.
The test covered one prompt with no grammar constraints and the
installed OpenCL backend. It did not combine these graph changes
with the earlier two-stage GPU reduction. The trial used scratch
binaries and did not change the installed worker.
