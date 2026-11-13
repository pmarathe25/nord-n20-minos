# Chunk distances

MinOS exposes view and simulation distance from 2 through 32 chunks. The former
12-chunk maximum was a MinOS policy, not a measured limit of this phone. Raising
the allowed maximum does not change the configured distances. Changes made
through the dashboard retain the existing restart-required behavior.

## Game limits

A read-only inspection of the installed vanilla 26.3 server JAR on 2026-11-09
found `ChunkMap.MIN_VIEW_DISTANCE = 2`, `MAX_VIEW_DISTANCE = 32`, and
`ChunkMap.setServerViewDistance(int)` clamping its argument to that range.
`getPlayerViewDistance` also clamps the player's requested distance to the
server's limit. A larger server value cannot force a client to request more.

Simulation distance is not clamped in the same setter: the properties loader
accepts an integer and `DistanceManager.updateSimulationDistance` stores it.
However, `getPlayerTicketLevel()` computes
`max(0, ChunkLevel.byStatus(ENTITY_TICKING) - simulationDistance)`, and
`ChunkLevel.ENTITY_TICKING_LEVEL` is 31. Thus the player simulation ticket
reaches its minimum level and stops expanding through this mechanism. Writing
an arbitrarily large configuration number does not produce unlimited ticking.
MinOS exposes the conventional range through 32 rather than accepting values
beyond the normal game configuration range. These observations apply to the
installed build; they should be rechecked when upgrading the server.

For comparison, [Paper's properties reference](https://docs.papermc.io/paper/reference/server-properties/)
documents a maximum of 32 for both properties. Paper is a different server
implementation; the vanilla inspection above is the evidence for this device.

## Performance and power

These settings change the radius in chunks, not the dimensions of each chunk.
View distance controls the world data sent to players. Simulation distance
controls active simulation around them. Mojang introduced separate simulation
distance specifically to permit greater viewing distance with less CPU work;
see the [original explanation](https://www.minecraft.net/en-us/article/minecraft-snapshot-21w38a).

An illustrative square footprint is `(2r + 1)^2` chunks per isolated player:

| Radius | Square footprint | Relative to radius 8 |
| --- | ---: | ---: |
| 8 | 289 | 1.0× |
| 12 | 625 | 2.2× |
| 16 | 1,089 | 3.8× |
| 24 | 2,401 | 8.3× |
| 32 | 4,225 | 14.6× |

This is geometry, not an exact loaded/ticking-chunk count, memory estimate,
or electrical-power multiplier. Actual chunk shapes, generation/loading
margins, overlapping players, client settings, forced chunks and game mechanics
change the workload. Separated players tend to add more unique chunks than
players together. Exploring new terrain introduces generation work in addition
to ordinary loading and simulation.

Higher view distance tends to add memory, loading/generation work and network
traffic. Higher simulation distance tends to add ongoing CPU work for active
game systems. The relationship to battery draw is workload-dependent; higher
distance can raise active power and temperature, but cannot be translated into
a reliable percentage from the area table. The pause-when-empty policy and
Wi-Fi power-saving policy are unchanged by the range expansion.

## Guidance for this phone

For general survival play, start with **24 view / 10 simulation**, using 8 as
a lower-load alternative. Simulation 10 preserves the usual default radius.
MinOS's reference configuration defaults to 32/10; 24/10 is a lower-load alternative.
Vanilla defaults to 10 view / 10 simulation; the two settings serve different purposes. See the
[server properties reference](https://docs.papermc.io/paper/reference/server-properties/).

Simulation 4 is an optional exploration or lower-load setting. Its roughly
64-block simulation radius is substantially smaller than the default's roughly
160 blocks. A high view distance keeps terrain visible beyond this radius, but
does not keep all distant mobs, farms and mechanisms active. Different game
systems have additional ticking and spawning rules; distance alone is not a
guarantee that a particular farm runs. Distant Horizons does not extend simulation.
These are starting points, not validated maxima for arbitrary worlds or players.
The [tuning comparison](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-09-server-tuning.md) evaluates ways to
improve the 24/10 workload without reducing simulation distance.

View 32 remained comfortable while stationary in the test world, but increased
new-terrain loading time, CPU use and exploration current, with terrain delivery
falling behind movement. View 16 / simulation 4 is a lighter alternative,
particularly for memory; a small 8–12 view is not an inherent requirement of
this phone. The client's render distance must also permit the desired view.
The UI exposes the larger range without silently changing existing settings.

The [distance benchmark](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-09-chunk-distances.md) now measures a simulated connected
player, stationary play and repeated movement into new terrain. It uses
disposable worlds and leaves production settings unchanged. Its limits matter:
there is no rendered client, player-built base, farm, combat or multi-player
workload, and the unplugged vendor-current comparison is a relative proxy
without calibrated electrical units.
It establishes neither a universal gameplay maximum nor a battery-life curve.
The [earlier measurements](https://github.com/pmarathe25/nord-n20-minos/blob/03af5bfb1d9afc5dd5a573855568d112fd42fe78/reports/2026-11-08-power-baseline.md) cover heap sizes and idle policies.

The benchmark also confirms that raising the vanilla configuration to 256
does not bypass the effective view-distance clamp. Detailed measurements belong
in the linked report.

For substantially longer vistas, existing software can help without writing a
new server. [Distant Horizons](https://modrinth.com/mod/distanthorizons) supports
256+ chunk views using simplified distant terrain. Client-only mode retains
previously explored terrain; compatible server support can send distant terrain
automatically. [Paper's maximum-view-distance override](https://docs.papermc.io/paper/reference/system-properties/#papermaxviewdistance)
can raise its server-side limit, but client limits and performance must still be
verified. See [server tuning](server-tuning.md) for the available controls.
