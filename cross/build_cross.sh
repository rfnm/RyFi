#!/bin/bash
# Cross-build RyFi (+ ryfi_bench) for the RFNM i.MX8MP (aarch64 Cortex-A53, NEON).
# Stages volk (static) and librfnm into cross/prefix, then builds RyFi against them.
# Verify with: qemu-aarch64 -L /usr/aarch64-linux-gnu build-aarch64/ryfi_bench 720e3 5
#
# Usage: cross/build_cross.sh <volk-src-dir> <librfnm-src-dir> <spdlog-src-dir> [libusb-rootfs=/r/mfs/current]
set -e
cd "$(dirname "$0")/.."

VOLK_SRC=${1:?volk source dir required}
LIBRFNM_SRC=${2:?librfnm source dir required}
SPDLOG_SRC=${3:?spdlog source dir required}
TOOLCHAIN=$PWD/cross/toolchain-aarch64-a53.cmake
PREFIX=$PWD/cross/prefix
export RYFI_CROSS_PREFIX=$PREFIX

# volk: static, no tests/profiling tools (they need a target python)
cmake -S "$VOLK_SRC" -B cross/build-volk -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX \
	-DENABLE_STATIC_LIBS=ON -DENABLE_MODTOOL=OFF -DENABLE_TESTING=OFF
cmake --build cross/build-volk -j"$(nproc)"
cmake --install cross/build-volk >/dev/null

# libusb (librfnm dependency): staged from the board rootfs - the exact target build.
# Copy only; never write into the rootfs (it is a live NFS root).
LIBUSB_ROOTFS=${4:-/r/mfs/current}
mkdir -p $PREFIX/lib $PREFIX/include/libusb-1.0 $PREFIX/lib/pkgconfig
cp -a "$LIBUSB_ROOTFS"/usr/lib/aarch64-linux-gnu/libusb-1.0.so* $PREFIX/lib/
cp -a "$LIBUSB_ROOTFS"/usr/lib/aarch64-linux-gnu/libudev.so.1* $PREFIX/lib/ # libusb DT_NEEDED
cp "$LIBUSB_ROOTFS"/usr/include/libusb-1.0/libusb.h $PREFIX/include/libusb-1.0/
cat > $PREFIX/lib/pkgconfig/libusb-1.0.pc <<PCEOF
prefix=$PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libusb-1.0
Description: C API for USB device access (staged aarch64 copy from the RFNM rootfs)
Version: 1.0.27
Libs: -L\${libdir} -lusb-1.0
Cflags: -I\${includedir}/libusb-1.0
PCEOF

# spdlog (librfnm dependency), static
cmake -S "$SPDLOG_SRC" -B cross/build-spdlog -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX \
	-DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build cross/build-spdlog -j"$(nproc)"
cmake --install cross/build-spdlog >/dev/null

# librfnm (>= 0.2.0)
PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig \
cmake -S "$LIBRFNM_SRC" -B cross/build-librfnm -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX \
	-Dspdlog_DIR=$PREFIX/lib/cmake/spdlog \
	-DBUILD_RFNM_LOCAL_TRANSPORT=ON
cmake --build cross/build-librfnm -j"$(nproc)"
cmake --install cross/build-librfnm >/dev/null

# RyFi + bench against the staged prefix
PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig \
cmake -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_PREFIX_PATH=$PREFIX \
	-DOPT_BUILD_BLADERF_SUPPORT=OFF -DOPT_BUILD_LIMESDR_SUPPORT=OFF \
	-DOPT_BUILD_USRP_SUPPORT=OFF -DOPT_BUILD_RFNM_SUPPORT=ON
cmake --build build-aarch64 -j"$(nproc)"

echo "aarch64 binaries: build-aarch64/ryfi build-aarch64/ryfi_bench"
