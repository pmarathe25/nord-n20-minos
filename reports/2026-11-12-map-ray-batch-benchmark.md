# Map ray batch timing — 2026-11-12

On the Nord N20, a separate Minecraft server ran at nice 19 with a 512 MiB maximum Java heap in a disposable flat world. The live server had two players. Each batch repeatedly checked three loaded air, glass and stone cells using the deployed ray-check commands, followed by a response marker; timings include command submission, server scheduling and observing the marker in the log. Eight measured runs per size followed a warmup, with order varied. This is not a direct CPU-time measurement, a long-distance test, or a gameplay tick-impact test.

| Checks | Median round trip | Minimum | Maximum |
| ---: | ---: | ---: | ---: |
| 0 | 40.58 ms | 21.44 ms | 67.38 ms |
| 3 | 53.63 ms | 36.52 ms | 70.07 ms |
| 6 | 56.63 ms | 39.06 ms | 72.99 ms |
| 12 | 60.76 ms | 21.78 ms | 71.19 ms |
| 24 | 56.02 ms | 46.06 ms | 112.88 ms |
| 48 | 100.98 ms | 61.73 ms | 137.94 ms |

The server tick and scheduling variability are large compared with differences between small batches, so these data cannot establish a precise per-check cost. They suggest testing moderately larger batches is worthwhile, but 48 checks had longer observed latency. The experiment did not test live gameplay performance or multiple concurrent ray requests.

The deployed bridge sends at most three checks in an atomic FIFO write (about 3,517 bytes including its marker, below the 4,096-byte PIPE_BUF). Four checks exceed that limit, so increasing the batch limit requires a protocol change or shorter commands to avoid interleaving with other requests. Alternatively, reducing the 50 ms pacing interval increases the maximum scheduling rate, but may put more work into a server tick and needs gameplay testing. The earlier end-to-end disposable-world trial took about 17 seconds for 513 checks, including all batch waits and response handling.

The disposable world and benchmark script were removed after the trial. Phone battery temperature was 28.1 C afterward.
