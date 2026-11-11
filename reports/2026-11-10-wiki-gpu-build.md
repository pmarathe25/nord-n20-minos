# Scratch GPU worker build check

2026-11-10. The non-deploying `tools/build-wiki-gpu.py` helper was run twice from a clean Git checkout of llama.cpp b12110 (`cd959bae99ff4bed22bef2604f45241c09bbb8c8`) with Android NDK r30, API 30, OpenCL target 200, and the six checkout patches. Both Android ARM64 builds completed. The final staged bundle contained nine server/backend binary files, five kernel sources and the upstream license; its manifest's 15 file hashes and six patch hashes were checked. The first build's server started with `--version` under the phone's existing Bionic linker and reported Clang 21 and Android aarch64. It did not load the model or run inference; this is not a latency, accuracy, gameplay or deployment check.

The helper rejected an already-existing output directory and a wrong upstream revision before creating a new output directory. The archive intentionally omits `.git`; consequently the server's own version output reports an unknown commit, while the external manifest records the checked commit. This is not a bit-reproducibility test. The manifest is partial provenance, not a substitute for fingerprinting the complete toolchain, model, firmware and runtime.

No installed phone file, Wiki enable marker, server configuration, world, firmware or boot image was changed. Scratch build trees, copied phone binaries, and build logs were removed after recording this result.

## Model and streaming API smoke check

A later fresh build from the helper ran the actual server and Bonsai 2 4B checkpoint on the phone under the Bionic linker, with `GPUOpenCL` selected and the tuned texture and two-column options configured. It used a new shader cache, a private Unix socket, the checkout's streaming chat client and a private offline copy of the Wiki cache. The build and launcher were not installed. Startup to model loaded and listening was about 120.4 seconds with the new shader cache.

Two held-out questions each received four retrieved passages. The observer-pulse question (`j10`) passed the automated fact and source checks in 34.062 seconds. The blast-furnace question (`j03`) returned a valid source but failed its automated fact check in 51.407 seconds; that case also failed the earlier matched evaluation. These are model-request times after retrieval and startup, not full in-game chat or gameplay measurements. This is a two-case compatibility smoke check, not a new speed comparison or evidence of general answer quality. It verifies that the fresh bundle can load the checkpoint and serve these streaming requests; the configured optional kernel dispatches were not individually traced in this test.

The guard observed at least 2,867,688 KiB available memory and at most 26.8 °C battery temperature, restored Minecraft and verified its status; CDSP returned offline with zero recorded crashes. Private prompts, answers, cache copies, binaries and logs were removed afterward.
