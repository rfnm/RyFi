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
