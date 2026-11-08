cmake_minimum_required(VERSION 3.20)

project(FobosDMRVoiceBackendGPL VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(FOBOSAPP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "Path to the FobosAPP checkout")
set(FOBOS_MBELIB_NEO_ROOT "${FOBOSAPP_ROOT}/third_party/mbelib-neo" CACHE PATH "Path to mbelib-neo")
set(FOBOS_OPENDMR_ROOT "${FOBOSAPP_ROOT}/third_party/softdmr" CACHE PATH "Path to OpenDMR/softdmr")

if(NOT EXISTS "${FOBOS_MBELIB_NEO_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "mbelib-neo was not found. Set FOBOS_MBELIB_NEO_ROOT or fetch the GPL backend dependency.")
endif()

set(MBELIB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MBELIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MBELIB_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(MBELIB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(MBELIB_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
add_subdirectory("${FOBOS_MBELIB_NEO_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/mbelib-neo" EXCLUDE_FROM_ALL)

add_library(fobos_dmr_voice_mbelib SHARED
    src/fobos_dmr_voice_backend_mbelib.cpp
)

target_include_directories(fobos_dmr_voice_mbelib
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/include"
)

target_compile_definitions(fobos_dmr_voice_mbelib
    PRIVATE
        FOBOS_DMR_VOICE_BACKEND_BUILD=1
)

target_link_libraries(fobos_dmr_voice_mbelib
    PRIVATE
        mbe_neo::mbe_static
)

set_target_properties(fobos_dmr_voice_mbelib PROPERTIES
    OUTPUT_NAME "fobos_dmr_voice_mbelib"
)

add_executable(fobos_dmr_voice_probe
    tools/fobos_dmr_voice_probe.cpp
)

target_include_directories(fobos_dmr_voice_probe
    PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/include"
)

target_link_libraries(fobos_dmr_voice_probe
    PRIVATE
        fobos_dmr_voice_mbelib
)

if(EXISTS "${FOBOS_OPENDMR_ROOT}/opendmr.h")
    set(FOBOS_OPENDMR_SOURCES
        "${FOBOS_OPENDMR_ROOT}/opendmr.cpp"
        "${FOBOS_OPENDMR_ROOT}/decoder/mbelib.c"
        "${FOBOS_OPENDMR_ROOT}/decoder/mbe_adaptive.c"
        "${FOBOS_OPENDMR_ROOT}/decoder/mbe_unvoiced_fft.c"
        "${FOBOS_OPENDMR_ROOT}/decoder/ambe3600x2450.c"
        "${FOBOS_OPENDMR_ROOT}/decoder/ambe_common.c"
        "${FOBOS_OPENDMR_ROOT}/decoder/ecc.c"
        "${FOBOS_OPENDMR_ROOT}/decoder/ecc_const.c"
        "${FOBOS_OPENDMR_ROOT}/decoder/pffft.c"
        "${FOBOS_OPENDMR_ROOT}/decoder/fftpack.c"
        "${FOBOS_OPENDMR_ROOT}/encoder/cgolay24128.cpp"
        "${FOBOS_OPENDMR_ROOT}/encoder/mbeenc.cpp"
        "${FOBOS_OPENDMR_ROOT}/encoder/aux_sub.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/basicop2.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/ch_encode.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/dc_rmv.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/dsp_sub.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/encode.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/imbe_vocoder.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/imbe_vocoder_impl.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/math_sub.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/pe_lpf.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/pitch_est.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/pitch_ref.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/qnt_sub.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/rand_gen.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/sa_encode.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/tbls.cc"
        "${FOBOS_OPENDMR_ROOT}/encoder/v_uv_det.cc"
    )

    add_library(opendmr_static STATIC ${FOBOS_OPENDMR_SOURCES})
    target_include_directories(opendmr_static
        PUBLIC
            "${FOBOS_OPENDMR_ROOT}"
            "${FOBOS_OPENDMR_ROOT}/decoder"
            "${FOBOS_OPENDMR_ROOT}/encoder"
    )
    target_compile_definitions(opendmr_static PRIVATE
        _CRT_SECURE_NO_WARNINGS=1
        _USE_MATH_DEFINES=1
        MBE_STATIC=1
    )
    if(MSVC)
        target_compile_options(opendmr_static PRIVATE /wd4244 /wd4267 /wd4305 /wd4996)
    endif()

    add_library(fobos_dmr_voice_opendmr SHARED
        src/fobos_dmr_voice_backend_opendmr.cpp
    )
    target_include_directories(fobos_dmr_voice_opendmr
        PUBLIC
            "${CMAKE_CURRENT_LIST_DIR}/include"
        PRIVATE
            "${FOBOS_OPENDMR_ROOT}"
    )
    target_compile_definitions(fobos_dmr_voice_opendmr
        PRIVATE
            FOBOS_DMR_VOICE_BACKEND_BUILD=1
    )
    target_link_libraries(fobos_dmr_voice_opendmr PRIVATE opendmr_static)
    set_target_properties(fobos_dmr_voice_opendmr PROPERTIES
        OUTPUT_NAME "fobos_dmr_voice_opendmr"
    )

    add_executable(fobos_dmr_voice_opendmr_probe
        tools/fobos_dmr_voice_probe.cpp
    )
    target_include_directories(fobos_dmr_voice_opendmr_probe
        PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/include"
    )
    target_compile_definitions(fobos_dmr_voice_opendmr_probe
        PRIVATE
            FOBOS_DMR_VOICE_PROBE_HAS_PAYLOAD_CHANNEL_CODE=1
    )
    target_link_libraries(fobos_dmr_voice_opendmr_probe
        PRIVATE
            fobos_dmr_voice_opendmr
    )
else()
    message(STATUS "OpenDMR/softdmr not found; encode-capable backend will not be built.")
endif()

install(TARGETS fobos_dmr_voice_mbelib
    RUNTIME DESTINATION dmr_voice_backends
    LIBRARY DESTINATION dmr_voice_backends
    ARCHIVE DESTINATION lib
)

install(FILES include/fobos_dmr_voice_backend.h
    DESTINATION include
)

if(TARGET fobos_dmr_voice_opendmr)
    install(TARGETS fobos_dmr_voice_opendmr
        RUNTIME DESTINATION dmr_voice_backends
        LIBRARY DESTINATION dmr_voice_backends
        ARCHIVE DESTINATION lib
    )
endif()
