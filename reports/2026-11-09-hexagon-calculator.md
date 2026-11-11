# Nord N20 Hexagon calculator, November 9, 2026

The CPH2459 compute DSP executed a custom v66 module through FastRPC on
MinOS. The ARM64 client sent operands and checked the output buffer returned
by the DSP. Both calls passed and the client exited zero.

| Input | Result |
| --- | ---: |
| 17 + 25 | 42 |
| 123456789 + 987654321 | 1111111110 |

The trial used the existing CDSP firmware, the C.24 vendor FastRPC client
library, and the matching v66 `fastrpc_shell_unsigned_3` read from the
existing `dsp_b` filesystem. The kernel booted CDSP and bound
`fastrpcglink-apps-dsp`. The vendor library created an unsigned user process
and used reverse RPC to read the calculator shared object. The custom module
ran within that user process. The firmware and partition metadata were
left in place.

Two earlier attempts reached module loading and returned `0x80000406`.
The diagnostic log showed that the client found the custom file. The
successful version used page-aligned load segments, a SysV symbol hash, and
the required `DT_HEXAGON_VER=3` entry. The Linux-oriented linker did not
emit that entry by default. These fixes were tested together.
[Qualcomm Hexagon ABI, §14.1](https://docs.qualcomm.com/doc/80-N2040-23/80-N2040-23_REV_K_Qualcomm_Hexagon_Application_Binary_Interface_User_Guide.pdf)

Minecraft saved and stopped through the existing console and maintenance
guard before each activation. The kernel logged an unsupported optional
FastRPC mapping operation and a shutdown-acknowledgment timeout. Both
calculator calls still completed, and CDSP returned to `OFFLINE` with zero
reported crashes. The phone did not reboot. The supervisor resumed
Minecraft with the whitelist enabled. The test establishes scalar DSP
execution. HVX correctness, throughput, and gameplay impact remain unmeasured.

## Rebuild the calculator

Build the Hexagon object with a compiler that supports v66 and link a
freestanding shared object. Reserve a dynamic entry with `-rpath /` and
replace that entry with the Hexagon ABI version tag.

```sh
zig cc -target hexagon-freestanding-none -mcpu=hexagonv66 -O2 \
    -ffreestanding -fPIC -c wiki/experiments/v66-calculator.c -o v66-calculator.o
ld.lld -shared -z defs --hash-style=sysv --no-rosegment -z max-page-size=4096 \
    -z common-page-size=4096 -z norelro -z separate-loadable-segments -Bsymbolic \
    --enable-new-dtags -rpath / v66-calculator.o -o libn20_calculator_skel.so
python3 wiki/experiments/tag-v66-elf.py libn20_calculator_skel.so
aarch64-linux-android30-clang -O2 wiki/experiments/v66-calculator.c -ldl -o n20-calculator
```

The ARM64 client uses the C.24 Bionic linker and the stock
`libcdsprpc.so` with its Android dependencies. The calculator and vendor
shell were staged in scratch storage visible through `ADSP_LIBRARY_PATH` and
`DSP_LIBRARY_PATH`, with the shell also available at `/vendor/dsp/cdsp` in
the client's private mount namespace. The trial held the CDSP subsystem
device open and waited for the RPMSG driver to bind before running the
client. Temporary FastRPC nodes used the major and minor numbers published
by the running kernel.
[Qualcomm FastRPC architecture](https://github.com/qualcomm/fastrpc)
