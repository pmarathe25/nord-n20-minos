# Server tuning

Tune for the actual player count and world. A terrain benchmark does not measure
the load from an established base, villagers, mob farms, redstone or several
players exploring in different directions. Preserve the existing screen,
Wi-Fi, charging, thermal, idle-pause and backup policies.

## Available controls

| Control | What changes | Tradeoff |
| --- | --- | --- |
| View distance | Full-detail chunks sent to each player | More generation, memory and network traffic for a wider view |
| Simulation distance | Active simulation around players | Lower settings can stop distant activity; independent of visible terrain |
| Pre-generation | When terrain generation happens | Moves work before play, consumes storage and time; loading/simulation remain |
| Server-only optimization mods | Implementation of simulation, generation and loading | Requires version-matched mods and validation against the real world's mechanics |
| JVM maximum heap | Memory available for Java objects | More memory is not automatically faster; leave room for native memory and the OS |
| Garbage collector | Allocation throughput and pause behavior | Measure tail tick times as well as average CPU use |
| Worker parallelism | Concurrent background generation/loading | Too many workers can compete with the main tick thread; too few can delay terrain |
| Network compression threshold | CPU spent compressing smaller packets versus bandwidth | Highly dependent on Wi-Fi and packet mix; do not disable blindly |
| Region compression | Chunk file size versus storage/CPU costs | Needs a separate save/load benchmark; does not remove generation work |
| Mob caps, entity activation, hopper intervals | Gameplay workload on implementations exposing these settings | Can change farm yields and game mechanics; not transparent optimizations |

The existing Java runtime is 25. Start with its default collector and the
current 512 MiB initial / 2 GiB maximum heap, then compare specific changes.
For the measured one-player terrain workload, four-processor pool sizing
improves the CPU/tail-latency tradeoff; adding Lithium provides a smaller
additional improvement. See the [tuning report](../reports/2026-11-09-server-tuning.md)
before applying those results to a different world or version.
[Java's collector guide](https://docs.oracle.com/en/java/javase/25/gctuning/available-collectors.html)
describes the throughput/latency tradeoffs. Avoid importing large collections
of JVM flags intended for much larger servers without measuring their effects.

`-XX:ActiveProcessorCount` changes Java's processor-dependent pool sizing;
it does not pin the process to the phone's fast cores. It affects multiple
subsystems, so a change cannot be attributed solely to world-generation threads.
See [Java's option reference](https://docs.oracle.com/en/java/javase/25/docs/specs/man/java.html).

[Lithium](https://modrinth.com/mod/lithium) targets game logic while aiming to
preserve vanilla mechanics and explicitly supports server-only installation.
[C2ME](https://modrinth.com/mod/c2me-fabric) parallelizes generation, I/O and
loading. Test the exact build; release labels and compatibility change over
time. A mod being faster in a short terrain test is not sufficient evidence to
replace the production server or validate every farm and save/recovery path.

## Distant Horizons

The [Distant Horizons project](https://modrinth.com/mod/distanthorizons) supports
client-only caching of explored terrain and server-assisted LOD delivery to
clients with compatible DH versions. LODs are simplified terrain for viewing;
they do not expand Minecraft simulation or require an equally large full-detail
server view distance.

A DH client can use a 256-chunk LOD radius while the server keeps a moderate
full-detail view and simulation radius. Clients without DH receive ordinary
Minecraft terrain. Keep other installed mods compatible with unmodded clients;
mods adding required blocks or protocol features can change that compatibility.

For this phone, evaluate cached or pre-generated LODs before enabling broad
on-demand generation. Bound the generation area, worker count, requests and
bandwidth. DH's [server-owner guidance](https://gitlab.com/distant-horizons-team/distant-horizons/-/wikis/1-user-guide/1-frequently-asked-questions/5-server-owners/Server-Owners)
describes these controls. A large LOD rendering radius is not proof that the
phone can generate that much terrain cheaply during play.

Use a real rendered DH client to evaluate horizon appearance, frame rate and
LOD delivery. The repository's headless client tests normal Minecraft server
work and optional-client compatibility; it does not request or render DH LODs.

See [distance guidance](chunk-distances.md), [benchmark instructions](benchmarking.md)
and [dated measurement reports](../reports/README.md). Keep results and version
pins in reports; retain only operating guidance here.
