# Block-by-block map ray trial, November 12, 2026

An isolated Minecraft Java 26.3 server ran on the phone at reduced CPU
priority in a disposable flat world, with no players and 33 force-loaded test
chunks. The ray used a captured armor-stand eye position after the stand had
been moved and rotated. It passed through clear, stained and tinted glass and a
stained pane, found a nearby stone block, a one-block stone target at 41 blocks,
and a stone target at 512 blocks, and reported no hit after those blocks were
removed. The capture entity was removed. The disposable world and logs were
deleted.

The test issued at most three cell checks per batch, with a 50 ms pause between
batches. Observed test-harness times were 0.26 seconds for five checks,
1.50 seconds for 42 checks, 17.05 seconds for the 513-cell maximum-range hit,
and 17.11 seconds for a 513-cell miss. The test harness polls for results and
the disposable server ran at a lower priority, so these are not controlled
production latency measurements. The trial did not measure live gameplay tick
latency, battery use, or a real player's rendered crosshair.

Checks still execute on Minecraft's server thread. The bridge's paced batches
limit each burst but do not guarantee stall-free gameplay. Unlike the earlier
coarse scan, the traversal checks every crossed block cell and stops without a
pin if a cell is unavailable. It still treats partial block shapes as full
cells and skips glass as configured.
