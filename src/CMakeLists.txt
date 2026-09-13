cmake_minimum_required(VERSION 3.16)
project(tetra_sniff C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Homebrew installs below one of these two prefixes. CMake searches neither one
# by itself.
if (APPLE)
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /usr/local)
endif ()

# SDR++ gives the DSP headers, and only the headers. Nothing of SDR++ is
# compiled, linked or installed. ./build.sh checks out the submodule.
if (NOT SDRPP_CORE_ROOT)
    set(SDRPP_CORE_ROOT ${ROOT}/third_party/sdrpp/core)
endif ()
if (NOT EXISTS ${SDRPP_CORE_ROOT}/src/dsp/channel/rx_vfo.h)
    message(FATAL_ERROR
        "The SDR++ core headers are missing at ${SDRPP_CORE_ROOT}.\n"
        "Run ./build.sh, which checks out the submodule, or pass "
        "-DSDRPP_CORE_ROOT=<sdrpp>/core")
endif ()

set(SRC ${ROOT}/sdrpp-tetra-demodulator/src)
set(DEC ${SRC}/decoder)
if (NOT EXISTS ${DEC}/src/tetra_common.h)
    message(FATAL_ERROR
        "The sdrpp-tetra-demodulator submodule is empty.\n"
        "Run ./build.sh, or: git submodule update --init --recursive")
endif ()
# ETSI does not allow redistribution of the speech codec, so no repository holds it.
if (NOT EXISTS ${DEC}/codec/c-code/cdec_tet.c)
    message(FATAL_ERROR
        "The ETSI speech codec source is missing.\n"
        "Run ./build.sh, or run "
        "sdrpp-tetra-demodulator/src/decoder/etsi_codec-patches/download_and_patch.sh")
endif ()

file(GLOB DEC_SRC LIST_DIRECTORIES false
    "${SRC}/dsp/*.cpp"
    "${DEC}/src/*.c"
    "${DEC}/src/crypto/*.c"
    "${DEC}/src/lower_mac/*.c"
    "${DEC}/src/phy/*.c")
foreach (f cdec_tet sub_cd tetra_op sdec_tet sub_sc_d sub_dsp fbas_tet fexp_tet fmat_tet)
    list(APPEND DEC_SRC "${DEC}/codec/c-code/${f}.c")
endforeach ()

find_path(VOLK_INCLUDE_DIR volk/volk.h)
find_library(VOLK_LIBRARY volk)
if (NOT VOLK_INCLUDE_DIR OR NOT VOLK_LIBRARY)
    if (APPLE)
        message(FATAL_ERROR "volk not found. Install it: brew install volk")
    else ()
        message(FATAL_ERROR "volk not found. Install it: sudo apt install libvolk-dev")
    endif ()
endif ()

find_library(RTLSDR_LIBRARY NAMES rtlsdr rtlsdr.0)
if (NOT RTLSDR_LIBRARY)
    if (APPLE)
        message(FATAL_ERROR "librtlsdr not found. Install it: brew install librtlsdr")
    else ()
        message(FATAL_ERROR "librtlsdr not found. Install it: sudo apt install librtlsdr-dev")
    endif ()
endif ()

add_executable(tetra-sniff main.cpp child.cpp recorder.cpp allocations.cpp rtl.cpp stitch.cpp sweep.cpp ${DEC_SRC})
target_include_directories(tetra-sniff PRIVATE . ${SRC} ${DEC}/src ${DEC}/codec ${SDRPP_CORE_ROOT}/src ${VOLK_INCLUDE_DIR})
target_link_libraries(tetra-sniff PRIVATE ${VOLK_LIBRARY} ${RTLSDR_LIBRARY})
# -fwrapv keeps signed overflow defined. L_add() in tetra_op.c detects overflow with it.
target_compile_options(tetra-sniff PRIVATE $<$<COMPILE_LANGUAGE:C>:-fwrapv;-O2>)
# The CLI lands at the top of the repository, so "./tetra-sniff run" works after a
# build. The $<1:...> wrapper stops a multi-config generator from adding a
# per-configuration directory below it.
set_target_properties(tetra-sniff PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${ROOT}>")

add_executable(gen_iq test/gen_iq.cpp)

add_executable(events_test test/events_test.cpp recorder.cpp allocations.cpp)
target_include_directories(events_test PRIVATE . ${DEC}/src)

add_executable(allocations_test test/allocations_test.cpp allocations.cpp)
target_include_directories(allocations_test PRIVATE .)

enable_testing()
add_test(NAME events_test COMMAND events_test)
add_test(NAME allocations_test COMMAND allocations_test)
