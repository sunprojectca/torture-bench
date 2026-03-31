cmake_minimum_required(VERSION 3.16)
project(torture-bench C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── detect architecture ────────────────────────────────────────────────────
include(CheckCCompilerFlag)

macro(append_supported_c_flag var flag test_name)
    check_c_compiler_flag("${flag}" ${test_name})
    if(${test_name})
        set(${var} "${${var}} ${flag}")
    endif()
endmacro()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(ARCH_ARM64 TRUE)
    message(STATUS "Architecture: ARM64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set(ARCH_X86_64 TRUE)
    message(STATUS "Architecture: x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64")
    set(ARCH_RISCV64 TRUE)
    message(STATUS "Architecture: RISC-V 64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64|powerpc64")
    set(ARCH_PPC64 TRUE)
    message(STATUS "Architecture: PowerPC64")
else()
    message(STATUS "Architecture: Generic (${CMAKE_SYSTEM_PROCESSOR})")
endif()

# ── OS detection ───────────────────────────────────────────────────────────
if(WIN32)
    message(STATUS "OS: Windows")
    add_definitions(-DOS_WINDOWS -D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN)
elseif(APPLE)
    message(STATUS "OS: macOS")
    add_definitions(-DOS_MACOS)
elseif(UNIX)
    message(STATUS "OS: Linux")
    add_definitions(-DOS_LINUX)
    add_compile_definitions(_GNU_SOURCE)
endif()

# ── compiler flags ─────────────────────────────────────────────────────────
# Use -O2 NOT -O3 — we want consistent fair results, not compiler heroics
if(MSVC)
    set(BASE_FLAGS "/O2 /W3")
    set(ARCH_FLAGS "")
else()
    set(BASE_FLAGS "-O2 -Wall -Wextra")
    append_supported_c_flag(BASE_FLAGS "-fno-builtin" HAS_FNO_BUILTIN)
    append_supported_c_flag(BASE_FLAGS "-fno-tree-vectorize" HAS_FNO_TREE_VECTORIZER)
    append_supported_c_flag(BASE_FLAGS "-fno-vectorize" HAS_FNO_VECTORIZER)
    append_supported_c_flag(BASE_FLAGS "-fno-slp-vectorize" HAS_FNO_SLP_VECTORIZER)
    set(ARCH_FLAGS "")
endif()

# Architecture-specific
if(ARCH_ARM64)
    add_definitions(-DARCH_ARM64)
    add_definitions(-DHAS_NEON)
    if(NOT MSVC)
        append_supported_c_flag(ARCH_FLAGS "-march=native" HAS_ARM64_MARCH_NATIVE)
        append_supported_c_flag(ARCH_FLAGS "-mcpu=native" HAS_ARM64_MCPU_NATIVE)
    endif()
elseif(ARCH_X86_64)
    add_definitions(-DARCH_X86_64)
    if(MSVC)
        # MSVC: check for AVX2 support
        check_c_compiler_flag("/arch:AVX2" HAS_AVX2)
        if(HAS_AVX2)
            add_definitions(-DHAS_AVX2)
        endif()
    else()
        check_c_compiler_flag("-mavx2" HAS_AVX2)
        if(HAS_AVX2)
            add_definitions(-DHAS_AVX2)
        endif()
        check_c_compiler_flag("-mavx512f" HAS_AVX512)
        if(HAS_AVX512)
            add_definitions(-DHAS_AVX512)
        endif()
        append_supported_c_flag(ARCH_FLAGS "-march=native" HAS_X86_MARCH_NATIVE)
    endif()
elseif(ARCH_RISCV64)
    add_definitions(-DARCH_RISCV64)
    if(NOT MSVC)
        append_supported_c_flag(ARCH_FLAGS "-march=rv64gc" HAS_RISCV_MARCH_RV64GC)
    endif()
elseif(ARCH_PPC64)
    add_definitions(-DARCH_PPC64)
    if(NOT MSVC)
        append_supported_c_flag(ARCH_FLAGS "-mcpu=native" HAS_PPC_MCPU_NATIVE)
    endif()
else()
    if(NOT MSVC)
        append_supported_c_flag(ARCH_FLAGS "-march=native" HAS_GENERIC_MARCH_NATIVE)
    endif()
endif()

set(CMAKE_C_FLAGS "${BASE_FLAGS} ${ARCH_FLAGS}")
set(CMAKE_C_FLAGS_RELEASE "${BASE_FLAGS} ${ARCH_FLAGS}")

# ── find pthreads (not needed on Windows — we use Win32 threads) ─────────
if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

# ── find math library ──────────────────────────────────────────────────────
if(NOT WIN32 AND NOT MSVC)
    find_library(MATH_LIBRARY m)
endif()

# ── module sources ─────────────────────────────────────────────────────────
set(MODULE_SOURCES
    modules/cpu_single.c
    modules/cpu_parallel.c
    modules/cpu_sustained.c
    modules/memory_bandwidth.c
    modules/memory_latency.c
    modules/cache_thrash.c
    modules/branch_chaos.c
    modules/hash_chain.c
    modules/raytracer.c
    modules/simd_dispatch.c
    modules/crypto_stress.c
    modules/ml_matmul.c
    modules/lattice_geometry.c
    modules/linear_algebra.c
    modules/exotic_chaos.c
    modules/ips_micro.c
    modules/pipeline_torture.c
    modules/ooo_execution.c
    modules/dependency_chain.c
    modules/speculation_stress.c
    modules/anticache_guard.c
)

# ── main benchmark binary ──────────────────────────────────────────────────
add_executable(torture-bench
    harness/main.c
    harness/orchestrator.c
    harness/reporter.c
    harness/platform.c
    ${MODULE_SOURCES}
)

if(WIN32)
    target_link_libraries(torture-bench Advapi32)
else()
    target_link_libraries(torture-bench
        Threads::Threads
        ${MATH_LIBRARY}
    )
endif()

target_include_directories(torture-bench PRIVATE
    ${CMAKE_SOURCE_DIR}/harness
    ${CMAKE_SOURCE_DIR}/modules
)

# ── tuning probe binary (run before main bench) ────────────────────────────
add_executable(tune-probe
    tools/tune_probe.c
    harness/platform.c
    modules/anticache_guard.c
)

if(WIN32)
    target_link_libraries(tune-probe Advapi32)
else()
    target_link_libraries(tune-probe Threads::Threads ${MATH_LIBRARY})
endif()

target_include_directories(tune-probe PRIVATE
    ${CMAKE_SOURCE_DIR}/harness
    ${CMAKE_SOURCE_DIR}/modules
)

install(TARGETS torture-bench tune-probe DESTINATION bin)
