cmake_minimum_required(VERSION 3.10)
project(ObriiSDR VERSION 6.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(POLICY CMP0072)
    cmake_policy(SET CMP0072 NEW)
endif()

if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
endif()

if(WIN32 AND NOT DEFINED Qt5_DIR AND EXISTS "C:/Qt/5.15.2/msvc2019_64/lib/cmake/Qt5")
    set(Qt5_DIR "C:/Qt/5.15.2/msvc2019_64/lib/cmake/Qt5" CACHE PATH "Qt5 CMake package directory")
endif()

find_package(Qt5 REQUIRED COMPONENTS Core Widgets Network SerialPort)
find_package(Qt5 COMPONENTS Multimedia QUIET)
if(NOT WIN32)
    find_package(OpenGL REQUIRED)
    find_package(Threads REQUIRED)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(FOBOS_STANDARD_ROOT "${CMAKE_SOURCE_DIR}/fobos" CACHE PATH "Path to standard libfobos headers/library")
set(FOBOS_AGILE_ROOT "${CMAKE_SOURCE_DIR}/fobos_agile" CACHE PATH "Path to agile libfobos_sdr headers/library")
set(FFTW_ROOT "${CMAKE_SOURCE_DIR}/fftw-3.3.5-dll64" CACHE PATH "Path to FFTW headers/library")

find_path(FOBOS_STANDARD_INCLUDE_DIR
    NAMES fobos.h
    HINTS "${FOBOS_STANDARD_ROOT}" "${FOBOS_STANDARD_ROOT}/include"
)
find_path(FOBOS_AGILE_INCLUDE_DIR
    NAMES fobos_sdr.h
    HINTS "${FOBOS_AGILE_ROOT}" "${FOBOS_AGILE_ROOT}/include"
)
find_path(FFTW_INCLUDE_DIR
    NAMES fftw3.h
    HINTS "${FFTW_ROOT}" "${FFTW_ROOT}/include"
)

find_library(FOBOS_STANDARD_LIBRARY
    NAMES fobos libfobos
    HINTS "${FOBOS_STANDARD_ROOT}" "${FOBOS_STANDARD_ROOT}/lib" "${FOBOS_STANDARD_ROOT}/x64" "${FOBOS_STANDARD_ROOT}/x86"
)
find_library(FOBOS_AGILE_LIBRARY
    NAMES fobos_sdr libfobos_sdr
    HINTS "${FOBOS_AGILE_ROOT}" "${FOBOS_AGILE_ROOT}/lib"
)
find_library(FFTW_FLOAT_LIBRARY
    NAMES fftw3f libfftw3f-3
    HINTS "${FFTW_ROOT}" "${FFTW_ROOT}/lib"
)
if(NOT WIN32)
    find_library(FFTW_FLOAT_THREADS_LIBRARY
        NAMES fftw3f_threads libfftw3f_threads
        HINTS "${FFTW_ROOT}" "${FFTW_ROOT}/lib"
    )
endif()

foreach(required_path
        FOBOS_STANDARD_INCLUDE_DIR
        FOBOS_AGILE_INCLUDE_DIR
        FFTW_INCLUDE_DIR
        FOBOS_STANDARD_LIBRARY
        FOBOS_AGILE_LIBRARY
        FFTW_FLOAT_LIBRARY)
    if(NOT DEFINED ${required_path} OR "${${required_path}}" STREQUAL "")
        message(FATAL_ERROR "Missing ${required_path}. Set FOBOS_STANDARD_ROOT, FOBOS_AGILE_ROOT, or FFTW_ROOT, or install the dependency system-wide.")
    endif()
endforeach()

option(FOBOS_ENABLE_MBELIB_NEO "Enable mbelib-neo DMR AMBE vocoder support" ON)
set(FOBOS_HAS_MBELIB_NEO OFF)
if(FOBOS_ENABLE_MBELIB_NEO AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/mbelib-neo/CMakeLists.txt")
    set(MBELIB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MBELIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(MBELIB_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(MBELIB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(MBELIB_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/mbelib-neo EXCLUDE_FROM_ALL)
    set(FOBOS_HAS_MBELIB_NEO ON)
else()
    message(STATUS "mbelib-neo not found or disabled; DMR voice synthesis will be unavailable.")
endif()

option(FOBOS_ENABLE_VKFFT "Enable experimental Vulkan VkFFT acceleration when the SDK is available" ON)
option(FOBOS_BUILD_GPU_FFT_SELFTEST "Build the VkFFT numerical comparison utility" OFF)
option(FOBOS_BUILD_CW_SELFTEST "Build the CW audio decoder self-test" OFF)
set(FOBOS_HAS_VKFFT OFF)
if(FOBOS_ENABLE_VKFFT AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/VkFFT/vkFFT/vkFFT.h")
    find_package(Vulkan QUIET COMPONENTS glslang SPIRV-Tools)
    find_library(VKFFT_SPIRV_TOOLS_OPT_LIBRARY
        NAMES SPIRV-Tools-opt
        HINTS "${Vulkan_INCLUDE_DIR}/../Lib" "${Vulkan_INCLUDE_DIR}/../lib"
    )
    if(Vulkan_FOUND AND Vulkan_glslang_FOUND AND Vulkan_SPIRV-Tools_FOUND
       AND TARGET Vulkan::glslang AND TARGET Vulkan::SPIRV-Tools
       AND VKFFT_SPIRV_TOOLS_OPT_LIBRARY)
        set(FOBOS_HAS_VKFFT ON)
        message(STATUS "VkFFT Vulkan backend enabled.")
    else()
        message(STATUS "Vulkan SDK with glslang not found; GPU FFT will fall back to FFTW.")
    endif()
else()
    message(STATUS "VkFFT source not found or disabled; GPU FFT will fall back to FFTW.")
endif()

set_source_files_properties(third_party/tiny_aes/aes.c PROPERTIES LANGUAGE CXX)
add_library(ObriiDmrCrypto STATIC src/digital/dmrprivacycodec.cpp third_party/tiny_aes/aes.c)
target_compile_definitions(ObriiDmrCrypto PRIVATE AES256=1 CBC=0 CTR=0 ECB=1)
target_include_directories(ObriiDmrCrypto PRIVATE src/digital src/utils third_party/tiny_aes)
target_link_libraries(ObriiDmrCrypto PRIVATE Qt5::Core)

add_library(ObriiItm STATIC
    third_party/ntia_itm/src/ComputeDeltaH.cpp
    third_party/ntia_itm/src/DiffractionLoss.cpp
    third_party/ntia_itm/src/FindHorizons.cpp
    third_party/ntia_itm/src/FreeSpaceLoss.cpp
    third_party/ntia_itm/src/FresnelIntegral.cpp
    third_party/ntia_itm/src/H0Function.cpp
    third_party/ntia_itm/src/InitializeArea.cpp
    third_party/ntia_itm/src/InitializePointToPoint.cpp
    third_party/ntia_itm/src/InverseComplementaryCumulativeDistributionFunction.cpp
    third_party/ntia_itm/src/itm_area.cpp
    third_party/ntia_itm/src/itm_p2p.cpp
    third_party/ntia_itm/src/KnifeEdgeDiffraction.cpp
    third_party/ntia_itm/src/LinearLeastSquaresFit.cpp
    third_party/ntia_itm/src/LineOfSightLoss.cpp
    third_party/ntia_itm/src/LongleyRice.cpp
    third_party/ntia_itm/src/QuickPfl.cpp
    third_party/ntia_itm/src/SigmaHFunction.cpp
    third_party/ntia_itm/src/SmoothEarthDiffraction.cpp
    third_party/ntia_itm/src/TerrainRoughness.cpp
    third_party/ntia_itm/src/TroposcatterLoss.cpp
    third_party/ntia_itm/src/ValidateInputs.cpp
    third_party/ntia_itm/src/Variability.cpp
)
set_target_properties(ObriiItm PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(ObriiItm PUBLIC ${CMAKE_SOURCE_DIR}/third_party/ntia_itm/include)

set(SOURCES
    src/main.cpp
    src/main.h
    src/mainwindow/main_appsettings.cpp
    src/mainwindow/main_controls.cpp
    src/mainwindow/main_devices.cpp
    src/mainwindow/main_digital.cpp
    src/mainwindow/main_ft8.cpp
    src/mainwindow/main_dmr.cpp
    src/mainwindow/main_dmr_privacy.cpp
    src/mainwindow/main_dspflow.cpp
    src/mainwindow/main_gnss.cpp
    src/mainwindow/main_gnss_acquisition.cpp
    src/mainwindow/main_gnss_acquisition_tests.cpp
    src/mainwindow/main_gnss_map.cpp
    src/mainwindow/main_gnss_satellites.cpp
    src/mainwindow/main_gnss_serial.cpp
    src/mainwindow/main_hardware_controls.cpp
    src/mainwindow/main_hackrf.cpp
    src/mainwindow/main_localization.cpp
    src/mainwindow/main_localization_translations.cpp
    src/mainwindow/main_measurements.cpp
    src/mainwindow/main_wireless.cpp
    src/mainwindow/main_propagation.cpp
    src/research/radiopropagation.cpp
    src/research/radiopropagation.h
    src/research/propagationlabdialog.cpp
    src/research/propagationlabdialog.h
    src/research/solarconditionswidget.cpp
    src/research/solarconditionswidget.h
    src/wireless/wirelesspackettools.cpp
    src/wireless/wirelesspackettools.h
    src/wireless/wirelesscaptureworker.cpp
    src/wireless/wirelesscaptureworker.h
    src/wireless/wirelesslabdialog.cpp
    src/wireless/wirelesslabdialog.h
    src/wireless/wirelesspacketdetails.cpp
    src/wireless/wirelesspacketdetails.h
    src/wireless/wifiphywidget.cpp
    src/wireless/wifiphywidget.h
    src/wireless/wifiliveiqworker.cpp
    src/wireless/wifiliveiqworker.h
    src/core/dmrcicdecimator.h
    src/mainwindow/main_network.cpp
    src/mainwindow/main_network_audio.cpp
    src/mainwindow/main_network_dialog.cpp
    src/mainwindow/main_network_iq.cpp
    src/mainwindow/main_network_spectrum.cpp
    src/mainwindow/main_presets.cpp
    src/mainwindow/main_recording.cpp
    src/mainwindow/main_scan.cpp
    src/mainwindow/main_scan_hunters.cpp
    src/mainwindow/main_settings.cpp
    src/mainwindow/main_settings_json.cpp
    src/mainwindow/main_settings_ui.cpp
    src/mainwindow/main_spectrum.cpp
    src/utils/appruntimeutils.cpp
    src/utils/appruntimeutils.h
    src/core/appdiagnostics.cpp
    src/core/appdiagnostics.h
    src/core/channelizerutils.cpp
    src/core/channelizerutils.h
    src/core/iqchannelizer.cpp
    src/core/iqchannelizer.h
    src/core/dspgraphmodel.cpp
    src/core/dspgraphmodel.h
    src/core/dspbranchengine.cpp
    src/core/dspbranchengine.h
    src/core/fftplannermutex.cpp
    src/utils/appconstants.h
    src/utils/appsettingsutils.cpp
    src/utils/appsettingsutils.h
    src/ui/apphelp.cpp
    src/ui/apphelp.h
    src/core/dataprocessor.cpp
    src/core/dataprocessor.h
    src/core/diagnosticlogging.h
    src/utils/dmrbackendpaths.cpp
    src/utils/dmrbackendpaths.h
    src/digital/dmrdecoder.cpp
    src/digital/dmrdecoder.h
    src/digital/dmrvocoder.cpp
    src/digital/dmrvocoder.h
    src/utils/dmrprivacyutils.cpp
    src/utils/dmrprivacyutils.h
    src/digital/dsdneobridge.cpp
    src/digital/dsdneobridge.h
    src/digital/gophertrunkbridge.cpp
    src/digital/gophertrunkbridge.h
    src/digital/dmrhunterdetector.cpp
    src/digital/dmrhunterdetector.h
    src/digital/digitalvideohunterdetector.cpp
    src/digital/digitalvideohunterdetector.h
    src/digital/digitaldecoder.cpp
    src/digital/digitaldecoder.h
    src/digital/dmrprivacyprocessor.cpp
    src/digital/dmrprivacyprocessor.h
    src/digital/ft8stationutils.cpp
    src/digital/ft8countrycatalog.cpp
    src/digital/ft8stationutils.h
    src/digital/ft8stationdialog.cpp
    src/digital/ft8stationservices.cpp
    src/digital/ft8stationdialog.h
    src/digital/cwaudiodecoder.cpp
    src/digital/psk31audiodecoder.cpp
    src/digital/psk31audiodecoder.h
    src/digital/cwaudiodecoder.h
    src/backends/bladerfbackend.cpp
    src/backends/bladerfbackend.h
    src/backends/hackrfbackend.cpp
    src/backends/hackrfbackend.h
    src/ui/frequencycontrol.cpp
    src/ui/frequencycontrol.h
    src/ui/finetunewidget.cpp
    src/ui/finetunewidget.h
    src/ui/multivfowidget.cpp
    src/ui/multivfowidget.h
    src/backends/fobosbackend.cpp
    src/backends/fobosbackend.h
    src/digital/fpvhunterdetector.cpp
    src/digital/fpvhunterdetector.h
    src/gnss/gnssacquisition.cpp
    src/gnss/gnssacquisition.h
    src/gnss/gnssqthhelpers.cpp
    src/gnss/gnssqthhelpers.h
    src/utils/gnssserialutils.cpp
    src/utils/gnssserialutils.h
    src/gnss/gnsssignalmonitor.cpp
    src/gnss/gnsssignalmonitor.h
    src/core/iqbuffer.cpp
    src/core/iqbuffer.h
    src/network/networkcontroller.cpp
    src/network/networkcontroller.h
    src/research/spectrumframerecorder.cpp
    src/research/spectrumframerecorder.h
    src/research/spectrumframereplaydialog.cpp
    src/research/spectrumframereplaydialog.h
    src/research/spectrumiqeventrecorder.cpp
    src/research/spectrumiqeventrecorder.h
    src/research/spectrumscienceanalyzer.cpp
    src/research/spectrumscienceanalyzer.h
    src/research/tonalspectrum.cpp
    src/gnss/gnsssdrstream.cpp
    src/gnss/gnsssdrstream.h
    src/gnss/gnsssdrpvtdialog.cpp
    src/gnss/gnsssdrpvtdialog.h
    src/digital/lrptdecodedialog.cpp
    src/digital/lrptdecodedialog.h
    src/mainwindow/main_externaltools.cpp
    src/research/tonalspectrum.h
    src/research/hdf5spectrumexport.cpp
    src/research/hdf5spectrumexport.h
    src/research/hdf5spectrumexportui.cpp
    src/research/hdf5spectrumexportui.h
    src/research/publicdatafetch.cpp src/research/publicdatafetch.h
    src/research/spaceeventsfeed.cpp src/research/spaceeventsfeed.h
    src/research/receivercalibrationtable.cpp
    src/research/receivercalibrationtable.h
    src/research/researchanalysisdialog.cpp
    src/research/researchanalysisdialog.h
    src/research/iqconstellationpreview.cpp
    src/research/iqconstellationpreview.h
    src/research/scientificsessiontools.cpp
    src/research/scientificsessiontools.h
    src/research/spectrumpersistencetools.cpp
    src/research/spectrumpersistencetools.h
    src/research/zerospandialog.cpp
    src/research/zerospandialog.h
    src/research/zoomspectrumprocessor.cpp
    src/research/zoomspectrumprocessor.h
    src/research/zoomspectrumdialog.cpp
    src/research/zoomspectrumdialog.h
    src/research/zoomdensitydialog.cpp
    src/research/zoomdensitydialog.h
    src/transmit/transmitterbackend.cpp
    src/transmit/transmitterbackend.h
    src/transmit/hackrftransmitterbackend.cpp
    src/transmit/hackrftransmitterbackend.h
    src/transmit/transmitwaveformgenerator.cpp
    src/transmit/transmitwaveformgenerator.h
    src/transmit/signallabgenerator.cpp
    src/transmit/signallabgenerator.h
    src/transmit/signallabdialog.cpp
    src/transmit/signallabdialog.h
    src/transmit/signallabpreview.cpp
    src/transmit/signallabpreview.h
    src/transmit/signallabconstellation3d.cpp
    src/transmit/signallabconstellation3d.h
    src/transmit/dmrtransmitencoder.cpp
    src/transmit/dmrtransmitencoder.h
    src/transmit/transmitmediagenerator.cpp
    src/transmit/transmitmediagenerator.h
    src/transmit/sstvimageeditordialog.cpp
    src/transmit/sstvimageeditordialog.h
    src/transmit/transmitdialog.cpp
    src/transmit/transmitdialog.h
    third_party/ft8_lib/ft8/constants.c
    third_party/ft8_lib/ft8/crc.c
    third_party/ft8_lib/ft8/encode.c
    third_party/ft8_lib/ft8/message.c
    third_party/ft8_lib/ft8/text.c
    src/audio/playbackmanager.cpp
    src/audio/playbackmanager.h
    src/audio/audiofilterchain.cpp
    src/audio/audiofilterchain.h
    src/presets/presethelpers.cpp
    src/presets/presethelpers.h
    src/presets/presetmanager.cpp
    src/gnss/qthlocator.cpp
    src/gnss/qthlocator.h
    src/gnss/qthmapwidget.cpp
    src/gnss/qthmapwidget.h
    src/audio/remoteaudioplayer.cpp
    src/audio/remoteaudioplayer.h
    src/audio/recordingmanager.cpp
    src/audio/recordingmanager.h
    src/core/radiosettings.h
    src/backends/receiverbackend.h
    src/backends/receiverbackendregistry.cpp
    src/backends/receiverbackendregistry.h
    src/utils/receiverdeviceutils.cpp
    src/utils/receiverdeviceutils.h
    src/backends/rtlsdrbackend.cpp
    src/backends/rtlsdrbackend.h
    src/utils/samplefileutils.cpp
    src/utils/samplefileutils.h
    src/ui/spectrumhuntercontrols.cpp
    src/ui/spectrumhuntercontrols.h
    src/ui/audiofilterchainwidget.cpp
    src/ui/audiofilterchainwidget.h
    src/ui/dspflowpanel.cpp
    src/ui/dspflowpanel.h
    src/ui/dspresearchwidget.cpp
    src/ui/dspresearchwidget.h
    src/digital/videoprocessor.cpp
    src/digital/videoprocessor.h
    src/digital/videowidget.cpp
    src/digital/videowidget.h
    src/core/fft.cpp
    src/core/fft.h
    src/core/gpufftbackend.cpp
    src/core/gpufftbackend.h
    src/utils/spectrumfftworker.cpp
    src/utils/spectrumfftworker.h
    src/utils/spectrumdisplayreducer.cpp
    src/utils/spectrumdisplayreducer.h
    src/audio/audioprocessor.cpp
    src/audio/audioprocessor.h
    src/ui/MyGraphWidget.cpp
    src/ui/MyGraphWidget.h
    src/ui/MyWaterfallWidget.cpp
    src/ui/MyWaterfallWidget.h
    src/ui/waterfall3drenderer.cpp
    src/ui/waterfall3drenderer.h
    src/ui/waterfall3dview.cpp
    src/ui/waterfall3dview.h
    src/utils/modulationutils.cpp
    src/utils/modulationutils.h
    src/ui/scalewidget.cpp
    src/ui/scalewidget.h
    src/scan/scanvisualassembler.cpp
    src/scan/scanvisualassembler.h
    src/utils/scanvisualutils.cpp
    src/utils/scanvisualutils.h
    src/backends/soapysdrbackend.cpp
    src/backends/soapysdrbackend.h
    src/utils/tuningutils.cpp
    src/utils/tuningutils.h
    src/app_resources.qrc
)

if(WIN32)
    list(APPEND SOURCES packaging/windows/fobosapp.rc)
endif()

add_executable(ObriiSDR ${SOURCES})
option(FOBOS_ENABLE_HF_PREDICTION "Optional isolated ITU HF planning dialog" ON)
if(FOBOS_ENABLE_HF_PREDICTION)
    add_subdirectory(src/propagation/ituhf)
    target_link_libraries(ObriiSDR PRIVATE $<LINK_ONLY:fobos_hf_prediction>)
    set_source_files_properties(src/research/propagationlabdialog.cpp PROPERTIES COMPILE_DEFINITIONS FOBOS_ENABLE_HF_PREDICTION=1)
endif()

if(MSVC)
    target_compile_options(ObriiSDR PRIVATE /utf-8)
endif()

target_compile_definitions(ObriiSDR PRIVATE FOBOSAPP_VERSION="${PROJECT_VERSION}")

target_include_directories(ObriiSDR PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/audio
    ${CMAKE_SOURCE_DIR}/src/backends
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/digital
    ${CMAKE_SOURCE_DIR}/src/gnss
    ${CMAKE_SOURCE_DIR}/src/mainwindow
    ${CMAKE_SOURCE_DIR}/src/network
    ${CMAKE_SOURCE_DIR}/src/presets
    ${CMAKE_SOURCE_DIR}/src/research
    ${CMAKE_SOURCE_DIR}/src/scan
    ${CMAKE_SOURCE_DIR}/src/ui
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/src/transmit
    ${CMAKE_SOURCE_DIR}/src/wireless
    ${CMAKE_SOURCE_DIR}/third_party/ft8_lib
    ${CMAKE_SOURCE_DIR}/FobosDMRVoiceBackend-gpl/include
    ${FOBOS_STANDARD_INCLUDE_DIR}
    ${FOBOS_AGILE_INCLUDE_DIR}
    ${FFTW_INCLUDE_DIR}
)

target_link_libraries(ObriiSDR PRIVATE
    ObriiDmrCrypto
    ObriiItm
    ${FOBOS_STANDARD_LIBRARY}
    ${FOBOS_AGILE_LIBRARY}
    ${FFTW_FLOAT_LIBRARY}
    Qt5::Core
    Qt5::Widgets
    Qt5::Network
    Qt5::SerialPort
)

if(FOBOS_HAS_VKFFT)
    target_include_directories(ObriiSDR PRIVATE
        "${CMAKE_SOURCE_DIR}/third_party/VkFFT/vkFFT"
        "${Vulkan_INCLUDE_DIR}/glslang/Include"
    )
    target_compile_definitions(ObriiSDR PRIVATE
        FOBOSAPP_HAS_VKFFT=1
        VKFFT_BACKEND=0
        VK_API_VERSION=11
    )
    set_source_files_properties(src/core/gpufftbackend.cpp PROPERTIES
        COMPILE_DEFINITIONS "_CRT_SECURE_NO_WARNINGS"
    )
    target_link_libraries(ObriiSDR PRIVATE
        Vulkan::Vulkan
        Vulkan::glslang
        Vulkan::SPIRV-Tools
        ${VKFFT_SPIRV_TOOLS_OPT_LIBRARY}
    )
    if(MSVC)
        target_link_options(ObriiSDR PRIVATE "/DELAYLOAD:vulkan-1.dll")
        target_link_libraries(ObriiSDR PRIVATE delayimp)
    endif()
endif()

if(FOBOS_HAS_VKFFT AND FOBOS_BUILD_GPU_FFT_SELFTEST)
    add_executable(FobosGpuFftSelfTest
        tools/vkfft_selftest.cpp
        src/core/gpufftbackend.cpp
        src/core/gpufftbackend.h
    )
    target_include_directories(FobosGpuFftSelfTest PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core"
        "${CMAKE_SOURCE_DIR}/third_party/VkFFT/vkFFT"
        "${Vulkan_INCLUDE_DIR}/glslang/Include"
        "${FFTW_INCLUDE_DIR}"
    )
    target_compile_definitions(FobosGpuFftSelfTest PRIVATE
        FOBOSAPP_HAS_VKFFT=1
        VKFFT_BACKEND=0
        VK_API_VERSION=11
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(FobosGpuFftSelfTest PRIVATE
        Qt5::Core
        ${FFTW_FLOAT_LIBRARY}
        Vulkan::Vulkan
        Vulkan::glslang
        Vulkan::SPIRV-Tools
        ${VKFFT_SPIRV_TOOLS_OPT_LIBRARY}
    )
endif()

find_path(OBRII_HDF5_INCLUDE_DIR hdf5.h
    HINTS "${CMAKE_SOURCE_DIR}/build/deps/hdf5-runtime/HDF5-1.14.6-win64/include"
    PATH_SUFFIXES hdf5/serial)
if(OBRII_HDF5_INCLUDE_DIR)
    target_include_directories(ObriiSDR PRIVATE ${OBRII_HDF5_INCLUDE_DIR})
    target_compile_definitions(ObriiSDR PRIVATE OBRII_HAS_HDF5_HEADERS=1)
endif()

if(FOBOS_BUILD_CW_SELFTEST)
    add_executable(ObriiScienceSelfTest tools/science_selftest.cpp
        src/research/tonalspectrum.cpp src/research/receivercalibrationtable.cpp
        src/research/spectrumscienceanalyzer.cpp src/research/spectrumframerecorder.cpp
        src/research/hdf5spectrumexport.cpp)
    target_include_directories(ObriiScienceSelfTest PRIVATE src/research src/core src/ui src/utils)
    target_link_libraries(ObriiScienceSelfTest PRIVATE Qt5::Core)
    if(OBRII_HDF5_INCLUDE_DIR)
        target_include_directories(ObriiScienceSelfTest PRIVATE ${OBRII_HDF5_INCLUDE_DIR})
        target_compile_definitions(ObriiScienceSelfTest PRIVATE OBRII_HAS_HDF5_HEADERS=1)
    endif()

    add_executable(ObriiPsk31SelfTest tools/psk31_selftest.cpp src/digital/psk31audiodecoder.cpp)
    add_executable(ObriiSpaceEventsSelfTest tools/spaceevents_selftest.cpp
        src/research/spaceeventsfeed.cpp src/research/spaceeventsfeed.h src/research/publicdatafetch.cpp
        src/research/solarconditionswidget.cpp src/research/solarconditionswidget.h src/utils/appsettingsutils.cpp)
    target_include_directories(ObriiSpaceEventsSelfTest PRIVATE src/research src/utils)
    target_link_libraries(ObriiSpaceEventsSelfTest PRIVATE Qt5::Widgets Qt5::Network)
    if(WIN32)
        target_link_libraries(ObriiSpaceEventsSelfTest PRIVATE winhttp)
    endif()
    if(MSVC)
        target_compile_options(ObriiSpaceEventsSelfTest PRIVATE /utf-8)
    endif()
    add_executable(ObriiExternalBackendSelfTest tools/external_backend_selftest.cpp
        src/gnss/gnsssdrstream.cpp src/gnss/gnsssdrstream.h
        src/gnss/gnsssdrpvtdialog.cpp src/gnss/gnsssdrpvtdialog.h
        src/digital/lrptdecodedialog.cpp src/digital/lrptdecodedialog.h
        src/core/iqbuffer.cpp src/audio/playbackmanager.cpp src/audio/playbackmanager.h src/utils/samplefileutils.cpp)
    target_include_directories(ObriiExternalBackendSelfTest PRIVATE src/gnss src/digital src/core src/audio src/utils)
    target_link_libraries(ObriiExternalBackendSelfTest PRIVATE Qt5::Widgets Qt5::Network)
    if(MSVC)
        target_compile_options(ObriiExternalBackendSelfTest PRIVATE /utf-8)
    endif()
    target_include_directories(ObriiPsk31SelfTest PRIVATE src/digital)
    target_link_libraries(ObriiPsk31SelfTest PRIVATE Qt5::Core)
    add_executable(ObriiDiagnosticLoggingSelfTest
        tools/diagnostic_logging_selftest.cpp src/core/appdiagnostics.cpp)
    target_include_directories(ObriiDiagnosticLoggingSelfTest PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
    target_link_libraries(ObriiDiagnosticLoggingSelfTest PRIVATE Qt5::Core)
    if(WIN32)
        target_link_libraries(ObriiDiagnosticLoggingSelfTest PRIVATE psapi)
    endif()

    add_executable(ObriiPropagationSelfTest
        tools/propagation_selftest.cpp
        src/research/radiopropagation.cpp
        src/research/propagationlabdialog.cpp src/research/propagationlabdialog.h
        src/utils/appsettingsutils.cpp
        src/research/solarconditionswidget.cpp src/research/solarconditionswidget.h
        src/research/publicdatafetch.cpp src/research/spaceeventsfeed.cpp src/research/spaceeventsfeed.h
        src/digital/ft8stationutils.cpp src/digital/ft8countrycatalog.cpp
        src/gnss/qthmapwidget.cpp src/gnss/qthmapwidget.h src/gnss/qthlocator.cpp
        src/app_resources.qrc)
    target_include_directories(ObriiPropagationSelfTest PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core
        ${CMAKE_SOURCE_DIR}/src/research ${CMAKE_SOURCE_DIR}/src/digital ${CMAKE_SOURCE_DIR}/src/gnss)
    target_include_directories(ObriiPropagationSelfTest PRIVATE ${CMAKE_SOURCE_DIR}/src/utils)
    target_link_libraries(ObriiPropagationSelfTest PRIVATE ObriiItm Qt5::Widgets Qt5::Network)
    if(FOBOS_ENABLE_HF_PREDICTION)
        target_link_libraries(ObriiPropagationSelfTest PRIVATE $<LINK_ONLY:fobos_hf_prediction>)
    endif()
    if(WIN32)
        target_link_libraries(ObriiPropagationSelfTest PRIVATE winhttp)
    endif()
    if(MSVC)
        target_compile_options(ObriiPropagationSelfTest PRIVATE /utf-8)
    endif()

    add_executable(ObriiDspFlowSelfTest
        tools/dsp_flow_selftest.cpp
        tools/dsp_branch_selftest.cpp
        src/ui/dspflowpanel.cpp src/ui/dspflowpanel.h
        src/ui/multivfowidget.cpp src/ui/multivfowidget.h
        src/ui/dspresearchwidget.cpp
        src/research/iqconstellationpreview.cpp
        src/transmit/signallabconstellation3d.cpp
        src/ui/finetunewidget.cpp src/ui/finetunewidget.h
        src/ui/MyWaterfallWidget.cpp src/ui/MyWaterfallWidget.h
        src/ui/waterfall3drenderer.cpp
        src/utils/appsettingsutils.cpp
        src/core/iqbuffer.cpp
        src/core/dspgraphmodel.cpp src/core/dspbranchengine.cpp
        src/core/iqchannelizer.cpp src/core/channelizerutils.cpp
        src/core/fftplannermutex.cpp
        src/ui/audiofilterchainwidget.cpp src/audio/audiofilterchain.cpp
        src/scan/scanvisualassembler.cpp)
    get_target_property(OBRII_DSP_TEST_INCLUDES ObriiSDR INCLUDE_DIRECTORIES)
    target_include_directories(ObriiDspFlowSelfTest PRIVATE ${OBRII_DSP_TEST_INCLUDES})
    target_link_libraries(ObriiDspFlowSelfTest PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets ${FFTW_FLOAT_LIBRARY})
    target_compile_definitions(ObriiDspFlowSelfTest PRIVATE OBRII_VFO_SELFTEST=1)
    if(WIN32)
        target_link_libraries(ObriiDspFlowSelfTest PRIVATE opengl32)
    else()
        target_link_libraries(ObriiDspFlowSelfTest PRIVATE OpenGL::GL Threads::Threads)
    endif()
    if(MSVC)
        target_compile_options(ObriiDspFlowSelfTest PRIVATE /utf-8)
    endif()

    add_executable(ObriiSpectrumPipelineSelfTest
        tools/spectrum_pipeline_selftest.cpp
        src/utils/spectrumdisplayreducer.cpp)
    target_include_directories(ObriiSpectrumPipelineSelfTest PRIVATE ${CMAKE_SOURCE_DIR}/src/utils)

    add_executable(ObriiIqChannelizerSelfTest tools/iq_channelizer_selftest.cpp
        src/core/iqchannelizer.cpp src/core/channelizerutils.cpp src/core/iqbuffer.cpp)
    target_include_directories(ObriiIqChannelizerSelfTest PRIVATE ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/utils)
    target_link_libraries(ObriiIqChannelizerSelfTest PRIVATE Qt5::Core)
    target_compile_definitions(ObriiIqChannelizerSelfTest PRIVATE OBRII_IQBUFFER_SELFTEST=1)
    if(NOT WIN32)
        target_link_libraries(ObriiIqChannelizerSelfTest PRIVATE Threads::Threads)
    endif()

    add_executable(ObriiWirelessDmrSelfTest
        tools/wireless_dmr_selftest.cpp
        src/wireless/wirelesspackettools.cpp
        src/wireless/wirelesscaptureworker.cpp
        src/wireless/wirelesscaptureworker.h
        src/wireless/wirelesslabdialog.cpp
        src/wireless/wirelesslabdialog.h
        src/wireless/wirelesspacketdetails.cpp
        src/wireless/wirelesspacketdetails.h
        src/wireless/wifiphywidget.cpp
        src/wireless/wifiphywidget.h
        src/wireless/wifiliveiqworker.cpp
        src/wireless/wifiliveiqworker.h
        src/digital/dsdneobridge.cpp
        src/digital/dsdneobridge.h
        src/utils/dmrprivacyutils.cpp
    )
    target_include_directories(ObriiWirelessDmrSelfTest PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/wireless
        ${CMAKE_SOURCE_DIR}/src/digital)
    target_include_directories(ObriiWirelessDmrSelfTest PRIVATE ${CMAKE_SOURCE_DIR}/src/utils)
    target_link_libraries(ObriiWirelessDmrSelfTest PRIVATE Qt5::Core Qt5::Widgets Qt5::Network)
    if(MSVC)
        target_compile_options(ObriiWirelessDmrSelfTest PRIVATE /utf-8)
    endif()

    add_executable(ObriiGopherDmrSelfTest
        tools/gopher_dmr_selftest.cpp
        src/digital/gophertrunkbridge.cpp src/digital/gophertrunkbridge.h
        src/digital/dmrdecoder.cpp src/digital/dmrdecoder.h)
    target_include_directories(ObriiGopherDmrSelfTest PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/digital)
    target_link_libraries(ObriiGopherDmrSelfTest PRIVATE Qt5::Core Qt5::Network)
    if(MSVC)
        target_compile_options(ObriiGopherDmrSelfTest PRIVATE /utf-8)
    endif()

    add_executable(ObriiDmrPipelineSelfTest
        tools/dmr_pipeline_selftest.cpp
        src/audio/audioprocessor.cpp src/audio/audioprocessor.h
        src/audio/audiofilterchain.cpp
        src/audio/recordingmanager.cpp src/audio/recordingmanager.h
        src/audio/playbackmanager.cpp src/audio/playbackmanager.h
        src/core/iqbuffer.cpp src/core/iqchannelizer.cpp src/core/channelizerutils.cpp)
    get_target_property(OBRII_DMR_TEST_INCLUDES ObriiSDR INCLUDE_DIRECTORIES)
    target_include_directories(ObriiDmrPipelineSelfTest PRIVATE ${OBRII_DMR_TEST_INCLUDES})
    target_link_libraries(ObriiDmrPipelineSelfTest PRIVATE Qt5::Core ${FFTW_FLOAT_LIBRARY})
    if(WIN32)
        target_link_libraries(ObriiDmrPipelineSelfTest PRIVATE winmm)
    else()
        target_link_libraries(ObriiDmrPipelineSelfTest PRIVATE Threads::Threads)
    endif()
    if(MSVC)
        target_compile_options(ObriiDmrPipelineSelfTest PRIVATE /utf-8)
    endif()

    add_executable(ObriiDmrPrivacySelfTest
        tools/dmr_privacy_selftest.cpp
        src/digital/dmrprivacyprocessor.cpp src/utils/dmrprivacyutils.cpp
        src/digital/dmrdecoder.cpp)
    target_include_directories(ObriiDmrPrivacySelfTest PRIVATE
        ${CMAKE_SOURCE_DIR}/src/digital ${CMAKE_SOURCE_DIR}/src/utils ${CMAKE_SOURCE_DIR}/src/core)
    target_compile_definitions(ObriiDmrPrivacySelfTest PRIVATE OBRII_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(ObriiDmrPrivacySelfTest PRIVATE Qt5::Core ObriiDmrCrypto)
    if(MSVC)
        target_compile_options(ObriiDmrPrivacySelfTest PRIVATE /utf-8)
    endif()

    add_executable(FobosCwDecoderSelfTest
        tools/cw_decoder_selftest.cpp
        src/digital/cwaudiodecoder.cpp
        src/digital/cwaudiodecoder.h
    )
    target_include_directories(FobosCwDecoderSelfTest PRIVATE
        ${CMAKE_SOURCE_DIR}/src/digital
    )
    target_link_libraries(FobosCwDecoderSelfTest PRIVATE Qt5::Core)
    if(MSVC)
        target_compile_options(FobosCwDecoderSelfTest PRIVATE /utf-8)
    endif()

    add_executable(FobosTransmitSelfTest
        tools/transmit_waveform_selftest.cpp
        src/digital/ft8stationutils.cpp
        src/digital/ft8countrycatalog.cpp
        src/app_resources.qrc
        src/gnss/qthlocator.cpp
        src/digital/ft8stationutils.h
        src/transmit/transmitwaveformgenerator.cpp
        src/transmit/transmitwaveformgenerator.h
        src/transmit/signallabgenerator.cpp
        src/transmit/signallabgenerator.h
        src/transmit/dmrtransmitencoder.cpp
        src/transmit/dmrtransmitencoder.h
        src/digital/dmrvocoder.cpp
        src/digital/dmrvocoder.h
        src/transmit/transmitmediagenerator.cpp
        src/transmit/transmitmediagenerator.h
        src/transmit/transmitterbackend.cpp
        src/transmit/transmitterbackend.h
        src/digital/videoprocessor.cpp
        src/digital/videoprocessor.h
        third_party/ft8_lib/ft8/constants.c
        third_party/ft8_lib/ft8/crc.c
        third_party/ft8_lib/ft8/encode.c
        third_party/ft8_lib/ft8/message.c
        third_party/ft8_lib/ft8/text.c
    )
    target_include_directories(FobosTransmitSelfTest PRIVATE
        ${CMAKE_SOURCE_DIR}/src/gnss
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/core
        ${CMAKE_SOURCE_DIR}/src/digital
        ${CMAKE_SOURCE_DIR}/src/transmit
        ${CMAKE_SOURCE_DIR}/src/utils
        ${CMAKE_SOURCE_DIR}/third_party/ft8_lib
        ${CMAKE_SOURCE_DIR}/FobosDMRVoiceBackend-gpl/include
        ${FFTW_INCLUDE_DIR}
    )
    target_link_libraries(FobosTransmitSelfTest PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets ${FFTW_FLOAT_LIBRARY})
    if(NOT WIN32)
        target_link_libraries(FobosTransmitSelfTest PRIVATE Threads::Threads)
    endif()
    if(MSVC)
        target_compile_options(FobosTransmitSelfTest PRIVATE /utf-8)
    endif()

    add_executable(ObriiSignalLabSelfTest tools/signal_lab_selftest.cpp
        src/transmit/signallabgenerator.cpp src/transmit/signallabpreview.cpp
        src/transmit/signallabconstellation3d.cpp
        src/research/iqconstellationpreview.cpp)
    target_include_directories(ObriiSignalLabSelfTest PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/transmit ${CMAKE_SOURCE_DIR}/src/research ${FFTW_INCLUDE_DIR})
    target_link_libraries(ObriiSignalLabSelfTest PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets ${FFTW_FLOAT_LIBRARY})
    if(NOT WIN32)
        target_link_libraries(ObriiSignalLabSelfTest PRIVATE Threads::Threads)
    endif()
    if(MSVC)
        target_compile_options(ObriiSignalLabSelfTest PRIVATE /utf-8)
    endif()

    add_executable(FobosSstvDecoderSelfTest
        tools/sstv_decoder_selftest.cpp
        src/digital/videoprocessor.cpp
        src/digital/videoprocessor.h
    )
    target_include_directories(FobosSstvDecoderSelfTest PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/core
        ${CMAKE_SOURCE_DIR}/src/digital
        ${CMAKE_SOURCE_DIR}/src/utils
    )
    target_link_libraries(FobosSstvDecoderSelfTest PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
    if(MSVC)
        target_compile_options(FobosSstvDecoderSelfTest PRIVATE /utf-8)
    endif()
endif()

if(WIN32)
    target_compile_definitions(ObriiSDR PRIVATE FOBOSAPP_HAS_FFTW_THREADS=1)
elseif(FFTW_FLOAT_THREADS_LIBRARY)
    target_link_libraries(ObriiSDR PRIVATE ${FFTW_FLOAT_THREADS_LIBRARY})
    target_compile_definitions(ObriiSDR PRIVATE FOBOSAPP_HAS_FFTW_THREADS=1)
else()
    message(STATUS "FFTW threads library not found; large FFT plans will remain single-threaded.")
endif()

if(FOBOS_HAS_MBELIB_NEO)
    target_link_libraries(ObriiSDR PRIVATE mbe_neo::mbe_static)
    target_compile_definitions(ObriiSDR PRIVATE FOBOSAPP_HAS_MBELIB_NEO=1)
endif()

if(WIN32)
    target_link_libraries(ObriiSDR PRIVATE winmm opengl32 winhttp)
else()
    target_link_libraries(ObriiSDR PRIVATE OpenGL::GL Threads::Threads)
endif()

if(Qt5Multimedia_FOUND)
    target_link_libraries(ObriiSDR PRIVATE Qt5::Multimedia)
    target_compile_definitions(ObriiSDR PRIVATE FOBOSAPP_HAS_QT_MULTIMEDIA=1)
    if(NOT WIN32)
        target_compile_definitions(ObriiSDR PRIVATE FOBOSAPP_HAS_QT_AUDIO=1)
    endif()
elseif(NOT WIN32)
    message(WARNING "Qt5 Multimedia was not found. Linux audio playback will be disabled until the Qt Multimedia development package is installed.")
endif()

install(TARGETS ObriiSDR RUNTIME DESTINATION bin)

if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/99-fobos-sdr.rules
            DESTINATION share/ObriiSDR/udev)
    install(FILES packaging/icons/fobosapp.png
            RENAME obriisdr.png
            DESTINATION share/icons/hicolor/256x256/apps)
    install(FILES packaging/linux/fobosapp.desktop.in
            RENAME ObriiSDR.desktop
            DESTINATION share/applications)
endif()
