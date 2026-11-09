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

For the owner's preference for wider vistas, start testing at 16 view distance
and 8 simulation distance, then try 24 view while keeping simulation at 6–8.
These are suggested trial settings, not validated gameplay maxima. Simulation
does not need to equal view distance: lowering it changes distant game activity
rather than directly shortening the visible terrain radius. A farm or another
build that needs distant simulation may justify a different tradeoff. The
client's own render distance must also permit the desired view.

An 8–12 view / 4–6 simulation configuration is a conservative, battery-oriented
alternative, not a universal recommendation for normal gameplay. The UI exposes
the larger range without silently increasing or reducing the user's settings.

There is no measured gameplay maximum or distance-versus-power curve for this
phone. The [existing measurements](power-baseline.md) compare heap sizes and
idle power policies, including a 64-force-loaded-chunk synthetic workload;
they are not a sweep of player view/simulation distances. The vendor battery
current reading is also uncalibrated.

A useful follow-up would compare 12/8, 16/8 and 24/8 with the same world, player
count, route and screen/power state, measuring server tick-time distribution,
memory, temperature, network traffic and externally measured energy. Include
both an established base and new-terrain exploration, repeat in alternate
order, and keep headroom below the 50 ms tick budget for 20 ticks per second.
Do not infer capacity from an idle server. No such gameplay or energy benchmark
was run as part of this UI range change.
