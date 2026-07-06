# Cross toolchain for the RFNM i.MX8MP (4x Cortex-A53, aarch64, NEON).
# -mcpu=cortex-a53 schedules for the in-order A53 pipeline; NEON is implied by
# aarch64. -fcx-limited-range keeps complex math out of the libgcc __mulsc3 slow
# path (harmless here, load-bearing wherever std::complex appears).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a53 -fcx-limited-range")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a53 -fcx-limited-range")

# staged cross deps (volk, spdlog, librfnm) land here via build_cross.sh
if(DEFINED ENV{RYFI_CROSS_PREFIX})
    list(APPEND CMAKE_FIND_ROOT_PATH "$ENV{RYFI_CROSS_PREFIX}")
endif()
list(APPEND CMAKE_FIND_ROOT_PATH "/usr/aarch64-linux-gnu")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
