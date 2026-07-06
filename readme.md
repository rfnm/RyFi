# DISCLAIMER: THIS PROTOCOL IS UNFINISHED AND SHOULD NOT BE USED ON AIR AS IS.

# RFNM port (2026-07-06)

This tree adds native RFNM support and an i.MX8MP (4x Cortex-A53, NEON) port:

- `src/device/rfnm.{h,cpp}` — full-duplex backend on librfnm (>= 0.2.1): one shared
  device context for the RX and TX halves (`-i rfnm:local -o rfnm:local`), CS16 on the
  wire converted at the edge, stale-channel recovery via the driver's canonical sweep,
  single-channel apply masks, 20 s reclock-safe apply timeouts, drop accounting from
  the transport counters. Identifiers: `rfnm:local`, `rfnm:usb[:serial]`, `rfnm:eth:IP`.
- `bench/ryfi_bench` — hardware-free loopback of the entire modem
  (packet→RS→conv→framing→RRC | AGC | FIR→demod→packet) with byte-exact payload
  verification and a throughput report; `[min-xrt]` gates the verdict on speed when run
  on the target CPU.
- `cross/` — toolchain + one-shot script cross-building volk (static, NEON), spdlog and
  librfnm (local transport ON) into a staging prefix, then RyFi + bench for the A53
  (`-mcpu=cortex-a53 -fcx-limited-range`). libusb/libudev are staged read-only from the
  board rootfs.

Verified off-board (boards were blocked):

- x86: `ryfi_bench 720e3 5 1.0` → 3594/3594 packets byte-exact, 10.5x realtime.
- qemu-aarch64 against the board rootfs sysroot: volk selects the `neonv8` machine
  (1856 NEON fma/mul in libvolk), `--drivers` lists rfnm, bench → 239/239 packets
  byte-exact. qemu throughput measures the emulator, so the speed gate is for the
  real A53: run `ryfi_bench 720e3 10 1.0` on the board at the next bench window,
  then profile before any hand-NEON work (qemu profiles mislead).

Board bring-up notes: needs librfnm >= 0.2.1 on the device (deploy together with the
fleet); mind the upstream disclaimer above before radiating - first RF tests belong on
a cable + attenuator between two boards.

## NEON pass (2026-07-06)

Thread-level profile of the loopback pipeline (x86, transfers structurally to the
A53): libcorrect Viterbi decode ~38% of CPU, PSK demod ~28% (RRC/AGC/Costas/clock
recovery - the latter three are serial per-sample feedback loops and cannot be
vectorized without changing the modem), everything else small.

- `libcorrect` Viterbi ACS rewritten as a shared helper with a NEON u16x8 path
  (aarch64): all 64 trellis states per time slice in vector lanes, key-table gather
  kept scalar, tie behavior identical to the historical loop. Bit-exact against the
  scalar reference (`LIBCORRECT_NO_NEON=1` selects scalar at runtime for A/B).
- `bench/conv_bench` measures the decoder alone and verifies the A/B: identical
  output checksums under qemu, modem loopback 244/244 byte-exact with NEON active.
- The RFNM backend's CS16<->float edge loops auto-vectorize at -O3 (verified in the
  aarch64 objdump).

qemu understates NEON gains (TCG emulates vector ops slowly; it showed only 1.1x) -
measure the real ratio on the A53 at the bench window:
`./conv_bench 200 4096` vs `LIBCORRECT_NO_NEON=1 ./conv_bench 200 4096`, then
`ryfi_bench 720e3 10 1.0`.

## Deep vectorization pass 2 (2026-07-06, ARM-only by design)

Per-stage re-profile after pass 1, then three targeted changes (deployment target is
the A53; x86 stays a plain scalar reference on purpose):

- **Costas loop**: libm `sinf`+`cosf` per sample replaced by `math::fastPhasor()`
  (folded least-squares polynomials, max error sin 1.0e-8 / cos 1.4e-7) - aarch64
  libm trig is expensive at sample rate, and the PLL wraps phase to [-pi, pi] so no
  general range reduction is needed.
- **`-fno-math-errno`** on all targets and in the cross toolchain: lets the compiler
  inline `sqrtf` (AGC amplitude tracking, MM recovery) to the hardware square-root
  instruction instead of a libm call.
- **Viterbi traceback search**: the per-time-slice 32-state argmin now runs in NEON
  u16x8 lanes with a lane-index shadow, preserving the scalar loop's lowest-state
  tie-break exactly.

Gates after the pass: x86 scalar reference bit-exact (conv checksum unchanged,
modem 9.7x realtime); qemu-aarch64 NEON-vs-scalar checksums identical, full modem
loopback byte-exact. volk kernel audit: every dot-product/magnitude/rotator kernel
the chain uses has NEON implementations; only trivial converters lack them (not in
the hot path). PSK's remaining scalar blocks (AGC gain update, MM feedback) are
serial by construction - further gains there mean changing the modem, not the code.
