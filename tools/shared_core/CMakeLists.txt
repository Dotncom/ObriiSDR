cmake_minimum_required(VERSION 3.16)
project(ObriiSharedCoreSelfTests LANGUAGES CXX)

get_filename_component(_obrii_default_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(OBRII_ROOT "${_obrii_default_root}" CACHE PATH "ObriiSDR source tree")
get_filename_component(OBRII_ROOT "${OBRII_ROOT}" ABSOLUTE)
if(NOT EXISTS "${OBRII_ROOT}/src/core/ffttransformcore.cpp")
    message(FATAL_ERROR "OBRII_ROOT must point to an ObriiSDR source tree")
endif()

find_package(Qt5 REQUIRED COMPONENTS Core Concurrent)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(FFTW_ROOT "" CACHE PATH "Optional float FFTW installation prefix")
find_path(FFTW_INCLUDE_DIR NAMES fftw3.h
    HINTS "${FFTW_ROOT}" "${FFTW_ROOT}/include"
          "${OBRII_ROOT}/fftw-3.3.5-dll64")
find_library(FFTW_FLOAT_LIBRARY NAMES fftw3f libfftw3f-3 fftw3f-3
    HINTS "${FFTW_ROOT}" "${FFTW_ROOT}/lib" "${FFTW_ROOT}/lib64"
          "${OBRII_ROOT}/fftw-3.3.5-dll64")
if(NOT FFTW_INCLUDE_DIR OR NOT FFTW_FLOAT_LIBRARY)
    message(FATAL_ERROR
        "Float FFTW is required. Install its development package or set FFTW_ROOT, "
        "FFTW_INCLUDE_DIR and FFTW_FLOAT_LIBRARY.")
endif()
add_library(ObriiSharedCoreFftw UNKNOWN IMPORTED)
set_target_properties(ObriiSharedCoreFftw PROPERTIES
    IMPORTED_LOCATION "${FFTW_FLOAT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${FFTW_INCLUDE_DIR}")

enable_testing()
function(obrii_shared_core_test target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    set_target_properties(${target} PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
    target_include_directories(${target} PRIVATE "${OBRII_ROOT}/src/core")
    target_link_libraries(${target} PRIVATE Qt5::Core Threads::Threads)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()

# The receiver test supplies its own safe-wrapper stubs, never driver libraries.
find_path(FOBOS_STANDARD_INCLUDE_DIR NAMES fobos.h
    HINTS "${OBRII_ROOT}/fobos" "${OBRII_ROOT}/fobos/include"
          "${OBRII_ROOT}/third_party/fobos-linux/include" /usr/local/include)
find_path(FOBOS_AGILE_INCLUDE_DIR NAMES fobos_sdr.h
    HINTS "${OBRII_ROOT}/fobos_agile" "${OBRII_ROOT}/fobos_agile/include"
          "${OBRII_ROOT}/third_party/fobos-linux/include" /usr/local/include)
if(FOBOS_STANDARD_INCLUDE_DIR AND FOBOS_AGILE_INCLUDE_DIR)
    add_executable(ObriiSharedReceiverCoreSelfTest
        "${OBRII_ROOT}/tools/shared_receiver_core_selftest.cpp")
    obrii_shared_core_test(ObriiSharedReceiverCoreSelfTest)
    target_include_directories(ObriiSharedReceiverCoreSelfTest PRIVATE
        "${FOBOS_STANDARD_INCLUDE_DIR}" "${FOBOS_AGILE_INCLUDE_DIR}")
else()
    message(WARNING
        "Skipping ObriiSharedReceiverCoreSelfTest: fobos.h and fobos_sdr.h are "
        "required. Set FOBOS_STANDARD_INCLUDE_DIR and FOBOS_AGILE_INCLUDE_DIR.")
endif()

add_executable(ObriiSharedFftCoreSelfTest
    "${OBRII_ROOT}/tools/shared_fft_core_selftest.cpp"
    "${OBRII_ROOT}/tools/fft_test_globals.cpp"
    "${OBRII_ROOT}/src/cli/clispectrummeter.cpp"
    "${OBRII_ROOT}/src/core/fft.cpp"
    "${OBRII_ROOT}/src/core/fft.h"
    "${OBRII_ROOT}/src/core/ffttransformcore.cpp"
    "${OBRII_ROOT}/src/core/gpufftbackend.cpp"
    "${OBRII_ROOT}/src/core/iqbuffer.cpp"
    "${OBRII_ROOT}/src/core/computebudgetmanager.cpp"
    "${OBRII_ROOT}/src/core/fftplannermutex.cpp")
obrii_shared_core_test(ObriiSharedFftCoreSelfTest)
target_include_directories(ObriiSharedFftCoreSelfTest PRIVATE "${OBRII_ROOT}/src/cli")
target_link_libraries(ObriiSharedFftCoreSelfTest PRIVATE Qt5::Concurrent ObriiSharedCoreFftw)

add_executable(ObriiSharedRecordingCoreSelfTest
    "${OBRII_ROOT}/tools/shared_recording_core_selftest.cpp"
    "${OBRII_ROOT}/src/core/recordingformatcore.cpp"
    "${OBRII_ROOT}/src/audio/recordingmanager.cpp"
    "${OBRII_ROOT}/src/audio/recordingmanager.h"
    "${OBRII_ROOT}/src/audio/playbackmanager.cpp"
    "${OBRII_ROOT}/src/audio/playbackmanager.h"
    "${OBRII_ROOT}/src/research/spectrumiqeventrecorder.cpp"
    "${OBRII_ROOT}/src/research/spectrumiqeventrecorder.h"
    "${OBRII_ROOT}/src/research/spectrumframerecorder.cpp"
    "${OBRII_ROOT}/src/research/spectrumframerecorder.h")
obrii_shared_core_test(ObriiSharedRecordingCoreSelfTest)
target_include_directories(ObriiSharedRecordingCoreSelfTest PRIVATE
    "${OBRII_ROOT}/src/audio" "${OBRII_ROOT}/src/research")
