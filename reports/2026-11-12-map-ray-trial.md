# Map ray trial, November 12, 2026

An isolated Minecraft Java 26.3 server ran on the phone with a disposable flat
world, no players, a two-chunk view distance and three force-loaded test chunks.
The test captured an armor stand's eye position, moved and rotated the stand,
then used the captured view to find synthetic blocks. Clear, tinted and stained
glass and a stained pane were skipped. Tests found the nearest target, a target
40 blocks away and a target 512 blocks away, exercised refinement, and verified
the temporary capture entity was removed. The world and raw logs were deleted.

The production algorithm checks 32 half-block samples through 16 blocks and 124
four-block samples thereafter: 156 coarse samples for the full 512-block range.
A distant coarse hit adds eight half-block refinement samples. In one run the
coarse scans took approximately 0.4 seconds and the refinement 0.2 seconds; a
512-sample, one-block-spacing comparison split into two 256-command batches
took approximately 0.22 and 0.42 seconds. The first cold scan took 1.01 seconds.
These include the test harness polling interval and are not a controlled
latency comparison. Most chunks along the line were not loaded. The trial did
not measure game tick stalls, fully loaded terrain, real players, battery or
long-term stability, and it cannot establish production responsiveness.

The coarse method can miss a thin obstacle between samples and find a different
block behind it; refinement only narrows the interval before a detected coarse
hit. The self-comparison block guard worked on the force-loaded test chunks;
`execute if loaded` was false in a non-ticking test chunk and was not used as
the eligibility check. The scan does not intentionally load chunks. The client
render distance and availability of server-side block data need not coincide.
