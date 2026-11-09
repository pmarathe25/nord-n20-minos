# Running disposable server benchmarks

Run from the original Windows checkout with Node.js and OpenSSH. The private
identity remains in ignored `local/`. This is explicitly a disruptive benchmark
and requires an idle, running production server and the existing accepted EULA.
Do not run it against an arbitrary server version or simultaneously with another
benchmark. Verify enough disk space and keep the SSH connection available.

```powershell
ssh -i local/secrets/n20_usb_key root@192.168.137.2 "mkdir -m 700 /run/n20/chunk-benchmark"
scp -i local/secrets/n20_usb_key tools/benchmark-chunk-server.py root@192.168.137.2:/run/n20/chunk-benchmark/
ssh -i local/secrets/n20_usb_key root@192.168.137.2 "python3 -m py_compile /run/n20/chunk-benchmark/benchmark-chunk-server.py"
node tests/test-chunk-client.js
node tools/benchmark-chunk-host.js --cases 12:8,16:8,24:8,32:8,256:8,16:4,16:16,16:32,16:8
```

`--durations 240,60,90` changes the three phase durations. `--client-view 127`
requests the largest positive value representable by the client's signed-byte
view-distance field, which is useful for checking a server cap independently
of the usual client request of 32. It does not enable a true 256-chunk view.
`N20_BENCHMARK_SCRIPT` can select a separately staged script under `/run/n20/`.
For a battery comparison, set `N20_BENCHMARK_HOST` to the phone's verified Wi-Fi
IPv4 address, physically disconnect external power, and add
`--require-unplugged --cases 24:8,24:4,32:8,24:8`. Keep the display off and the
phone on the same surface. This does not change charging or Wi-Fi policies.

Keep the summarized measurements and reusable tools; discard temporary worlds,
raw logs, downloaded protocol data and scratch measurement files after review.
The server harness deletes its temporary worlds automatically.

## Tuning comparisons

Use `--cases 24:8 --profiles baseline,heap3g,parallelgc,cpu4 --revisit 60`
to compare named JVM profiles and retrace the generated route. The baseline
uses the installed runtime's default collector with a 512 MiB initial / 2 GiB
maximum heap. `heap3g` changes only the maximum to 3 GiB; `parallelgc` selects
Parallel GC at 2 GiB. `cpu4` makes Java size its processor-dependent pools for
four processors. It neither pins threads to particular cores nor changes the
phone's CPU governor, and it changes more than just generation parallelism.

The revisit phase moves backwards along the same route at the same speed. It
measures an immediately revisited, partly memory-cached world, not cold disk
loading or a complete pre-generation strategy. Use matching exploration and
revisit durations to compare equivalent movement.

For isolated mod comparisons, stage and run `tools/prepare-benchmark-mods.py
--prepare` on the phone before measurements. It downloads pinned Minecraft
26.3 Fabric assets to `/tmp/n20-tuning-assets`, verifies mod and installer
hashes, and runs the installer without root privileges. It refuses to overwrite
that path. It does not start Minecraft or alter the production installation.
Fabric's installer fetches its own declared loader dependencies.

Profiles `lithium`, `c2me`, and `dh` use respectively Lithium, Lithium + C2ME,
and Lithium + Distant Horizons, with default mod settings and the baseline heap.
`lithium-cpu4` combines Lithium with the four-processor pool-sizing flag.
`dh-cpu4` adds Distant Horizons to that combination.
The simulated client has no mods and does not request DH LODs. The DH profile
can check optional-client compatibility and ordinary gameplay/background
overhead, but does not exercise DH-client requests for distant generation,
LOD transmission, rendering or client memory. DH may still build LODs from
ordinary loaded chunks. This cannot prove performance of a 256-chunk DH horizon.

Record version pins, case order, temperatures, transport and the power-source
state in a dated `reports/` file. Keep raw output in ignored `local/` while
reviewing, then remove the scratch assets and raw output. Do not commit binary
mods, world data or logs, or silently promote experimental profiles to production.
