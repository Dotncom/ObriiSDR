cmake_minimum_required(VERSION 3.10)
project(ObriiCli LANGUAGES C CXX)

set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/tiny_aes/aes.c"
    PROPERTIES LANGUAGE CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

get_filename_component(OBRII_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(WIN32 AND NOT DEFINED Qt5_DIR AND EXISTS "C:/Qt/5.15.2/msvc2019_64/lib/cmake/Qt5")
    set(Qt5_DIR "C:/Qt/5.15.2/msvc2019_64/lib/cmake/Qt5" CACHE PATH "Qt5 CMake package directory")
endif()
find_package(Qt5 REQUIRED COMPONENTS Core Network Gui Concurrent)
find_package(Qt5 COMPONENTS Multimedia QUIET)
find_package(Qt5 COMPONENTS Widgets QUIET)
add_library(ObriiUsrpLabCore STATIC
    "${OBRII_ROOT}/src/core/usrplabjob.cpp" "${OBRII_ROOT}/src/core/usrplabjob.h"
    "${OBRII_ROOT}/src/core/usrplabresources.qrc")
set_target_properties(ObriiUsrpLabCore PROPERTIES AUTORCC ON)
target_include_directories(ObriiUsrpLabCore PUBLIC "${OBRII_ROOT}/src/core")
target_link_libraries(ObriiUsrpLabCore PUBLIC Qt5::Core)
add_library(ObriiCommandCore STATIC "${OBRII_ROOT}/src/core/commanddispatcher.cpp")
target_include_directories(ObriiCommandCore PUBLIC "${OBRII_ROOT}/src/core")
target_link_libraries(ObriiCommandCore PUBLIC Qt5::Core)
set_target_properties(ObriiCommandCore PROPERTIES AUTOMOC OFF)
add_library(ObriiCommandNetwork STATIC "${OBRII_ROOT}/src/network/commandcontrolserver.cpp"
    "${OBRII_ROOT}/src/network/commandcontrolserver.h")
target_include_directories(ObriiCommandNetwork PUBLIC "${OBRII_ROOT}/src/network")
target_link_libraries(ObriiCommandNetwork PUBLIC ObriiCommandCore Qt5::Network)
if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

set(FOBOS_STANDARD_ROOT "${OBRII_ROOT}/fobos" CACHE PATH "Standard Fobos library prefix")
set(FOBOS_AGILE_ROOT "${OBRII_ROOT}/fobos_agile" CACHE PATH "Agile Fobos library prefix")
set(FFTW_ROOT "${OBRII_ROOT}/fftw-3.3.5-dll64" CACHE PATH "FFTW library prefix")

find_path(FOBOS_STANDARD_INCLUDE_DIR NAMES fobos.h
    HINTS "${FOBOS_STANDARD_ROOT}" "${FOBOS_STANDARD_ROOT}/include")
find_path(FOBOS_AGILE_INCLUDE_DIR NAMES fobos_sdr.h
    HINTS "${FOBOS_AGILE_ROOT}" "${FOBOS_AGILE_ROOT}/include")
find_path(FFTW_INCLUDE_DIR NAMES fftw3.h
    HINTS "${FFTW_ROOT}" "${FFTW_ROOT}/include")
find_library(FOBOS_STANDARD_LIBRARY NAMES fobos libfobos
    HINTS "${FOBOS_STANDARD_ROOT}" "${FOBOS_STANDARD_ROOT}/lib" "${FOBOS_STANDARD_ROOT}/x64")
find_library(FOBOS_AGILE_LIBRARY NAMES fobos_sdr libfobos_sdr
    HINTS "${FOBOS_AGILE_ROOT}" "${FOBOS_AGILE_ROOT}/lib")
find_library(FFTW_FLOAT_LIBRARY NAMES fftw3f libfftw3f-3
    HINTS "${FFTW_ROOT}" "${FFTW_ROOT}/lib")
foreach(dependency
        FOBOS_STANDARD_INCLUDE_DIR FOBOS_AGILE_INCLUDE_DIR FFTW_INCLUDE_DIR
        FOBOS_STANDARD_LIBRARY FOBOS_AGILE_LIBRARY FFTW_FLOAT_LIBRARY)
    if(NOT ${dependency})
        message(FATAL_ERROR "Missing ${dependency}; set FOBOS_STANDARD_ROOT, FOBOS_AGILE_ROOT, or FFTW_ROOT")
    endif()
endforeach()

add_executable(ObriiCli
    obriicli.cpp climodulemanager.cpp climodulemanager.h climoduleprotocol.h
    climcp.cpp climcp.h
    cliasyncspectrum.cpp cliasyncspectrum.h
    clidoctor.cpp clidoctor.h cliruntimerecorder.cpp cliruntimerecorder.h
    "${OBRII_ROOT}/src/utils/dmrprivacyutils.cpp"
    clivirtualiqsource.cpp clivirtualiqsource.h
    clicalibration.cpp clicalibration.h
    "${OBRII_ROOT}/src/research/receivercalibrationtable.cpp"
    "${OBRII_ROOT}/src/research/receivernoisebaseline.cpp"
    "${OBRII_ROOT}/src/research/receiveryfactorcalibration.cpp"
    cliaudiooutput.cpp cliaudiooutput.h
    clivfomanager.cpp clivfomanager.h
    clifrequencyscanner.cpp clifrequencyscanner.h
    cliiqtrigger.cpp cliiqtrigger.h
    cliiqquality.cpp cliiqquality.h
    clizerospan.cpp clizerospan.h
    clidspgraph.cpp clidspgraph.h
    climultisdr.cpp climultisdr.h
    cliwebservice.cpp cliwebservice.h
    clignsssession.cpp clignsssession.h
    cliresearchtools.cpp cliresearchtools.h
    "${OBRII_ROOT}/src/research/headlessresearchmetrics.cpp"
    "${OBRII_ROOT}/src/network/remotewebserver.cpp"
    "${OBRII_ROOT}/src/network/remotewebserver.h"
    "${OBRII_ROOT}/src/network/remotevfoservice.cpp"
    "${OBRII_ROOT}/src/network/remotevfoservice.h"
    "${OBRII_ROOT}/src/core/dspstreammanager.cpp"
    "${OBRII_ROOT}/src/core/dspconnectionprobe.cpp"
    "${OBRII_ROOT}/src/core/spectrumcomputesession.cpp"
    "${OBRII_ROOT}/src/core/fft.cpp" "${OBRII_ROOT}/src/core/fft.h"
    "${OBRII_ROOT}/src/core/gpufftbackend.cpp"
    "${OBRII_ROOT}/tools/fft_test_globals.cpp"
    "${OBRII_ROOT}/src/analyzer/analyzerfobosglobals.cpp"
    "${OBRII_ROOT}/src/analyzer/analyzerreceiver.cpp"
    "${OBRII_ROOT}/src/analyzer/analyzerreceiver.h"
    "${OBRII_ROOT}/src/analyzer/spectrummosaicengine.cpp"
    "${OBRII_ROOT}/src/analyzer/spectrummosaicengine.h"
    "${OBRII_ROOT}/src/backends/fobosbackend.cpp"
    "${OBRII_ROOT}/src/backends/fobosbackend.h"
    "${OBRII_ROOT}/src/backends/hackrfbackend.cpp"
    "${OBRII_ROOT}/src/backends/hackrfbackend.h"
    "${OBRII_ROOT}/src/core/dspgraphmodel.cpp"
    "${OBRII_ROOT}/src/core/dspbranchengine.cpp"
    "${OBRII_ROOT}/src/core/dspstreamprocessor.cpp"
    "${OBRII_ROOT}/src/core/dspstreamrecorder.cpp"
    "${OBRII_ROOT}/src/core/dspiqcodec.cpp"
    "${OBRII_ROOT}/src/core/dspsourceidentity.cpp"
    "${OBRII_ROOT}/src/core/dspiqsourceregistry.cpp"
    "${OBRII_ROOT}/src/core/computebudgetmanager.cpp"
    "${OBRII_ROOT}/src/core/pfbchannelizer.cpp"
    "${OBRII_ROOT}/src/core/fftplannermutex.cpp"
    clitextdecoder.cpp clitextdecoder.h
    "${OBRII_ROOT}/src/digital/cwaudiodecoder.cpp"
    "${OBRII_ROOT}/src/digital/cwaudiodecoder.h"
    "${OBRII_ROOT}/src/digital/psk31audiodecoder.cpp"
    "${OBRII_ROOT}/src/digital/psk31audiodecoder.h"
    "${OBRII_ROOT}/src/backends/soapysdrbackend.cpp"
    "${OBRII_ROOT}/src/backends/soapysdrbackend.h"
    "${OBRII_ROOT}/src/backends/uhdbackend.cpp"
    "${OBRII_ROOT}/src/backends/uhdbackend.h"
    "${OBRII_ROOT}/src/backends/rtlsdrbackend.cpp"
    "${OBRII_ROOT}/src/backends/rtlsdrbackend.h"
    clispectrummeter.cpp clispectrummeter.h
    clinetworkbridge.cpp clinetworkbridge.h
    "${OBRII_ROOT}/src/research/spectrumframerecorder.cpp"
    "${OBRII_ROOT}/src/research/spectrumframerecorder.h"
    "${OBRII_ROOT}/src/research/spectrumiqeventrecorder.cpp"
    "${OBRII_ROOT}/src/research/spectrumiqeventrecorder.h"
    "${OBRII_ROOT}/src/research/spectrumscienceanalyzer.cpp"
    "${OBRII_ROOT}/src/research/spectrumscienceanalyzer.h"
    "${OBRII_ROOT}/src/research/tonalspectrum.cpp"
    "${OBRII_ROOT}/src/research/tonalspectrum.h"
    "${OBRII_ROOT}/src/network/networkcontroller.cpp"
    "${OBRII_ROOT}/src/network/networkcontroller.h"
    "${OBRII_ROOT}/src/network/displayspectrumcodec.cpp"
    "${OBRII_ROOT}/src/network/displayspectrumcodec.h"
    "${OBRII_ROOT}/src/audio/audioprocessor.cpp"
    "${OBRII_ROOT}/src/audio/audioprocessor.h"
    "${OBRII_ROOT}/src/audio/audiofilterchain.cpp"
    "${OBRII_ROOT}/src/audio/remoteaudioplayer.cpp"
    "${OBRII_ROOT}/src/audio/remoteaudioplayer.h"
    "${OBRII_ROOT}/src/core/iqbuffer.cpp"
    "${OBRII_ROOT}/src/core/iqchannelizer.cpp"
    "${OBRII_ROOT}/src/core/channelizerutils.cpp"
    "${OBRII_ROOT}/src/openradio/virtualsignalframe.cpp"
    "${OBRII_ROOT}/src/openradio/virtualsignaltransport.cpp")
target_include_directories(ObriiCli PRIVATE
    "${OBRII_ROOT}/src/utils"
    "${OBRII_ROOT}/src/analyzer"
    "${OBRII_ROOT}/src/backends"
    "${OBRII_ROOT}/src/audio" "${OBRII_ROOT}/src/core"
    "${OBRII_ROOT}/src/digital"
    "${OBRII_ROOT}/src/network"
    "${OBRII_ROOT}/src/research"
    "${OBRII_ROOT}/src/ui"
    "${OBRII_ROOT}/src/openradio" "${FOBOS_STANDARD_INCLUDE_DIR}"
    "${FOBOS_AGILE_INCLUDE_DIR}" "${FFTW_INCLUDE_DIR}")
target_link_libraries(ObriiCli PRIVATE
    "${FOBOS_STANDARD_LIBRARY}" "${FOBOS_AGILE_LIBRARY}"
    "${FFTW_FLOAT_LIBRARY}" Qt5::Core Qt5::Network Qt5::Concurrent)
if(WIN32)
    target_link_libraries(ObriiCli PRIVATE winmm)
else()
    target_link_libraries(ObriiCli PRIVATE Threads::Threads)
    if(Qt5Multimedia_FOUND)
        target_link_libraries(ObriiCli PRIVATE Qt5::Multimedia)
        target_compile_definitions(ObriiCli PRIVATE FOBOSAPP_HAS_QT_AUDIO=1)
    endif()
endif()
if(MSVC)
    target_compile_options(ObriiCli PRIVATE /utf-8)
endif()

add_executable(ObriiCliModule
    climodulemanager.cpp climodulemanager.h
    obriiclimodule.cpp climoduleprotocol.h
    "${OBRII_ROOT}/src/digital/digitaldecoder.cpp"
    "${OBRII_ROOT}/src/digital/digitaldecoder.h"
    "${OBRII_ROOT}/src/digital/dmrdecoder.cpp"
    "${OBRII_ROOT}/src/digital/dmrdecoder.h"
    "${OBRII_ROOT}/src/digital/dmrvocoder.cpp"
    "${OBRII_ROOT}/src/digital/dmrvocoder.h"
    "${OBRII_ROOT}/src/digital/dmrprivacyprocessor.cpp"
    "${OBRII_ROOT}/src/digital/dmrprivacyprocessor.h"
    "${OBRII_ROOT}/src/digital/dmrprivacycodec.cpp"
    "${OBRII_ROOT}/src/digital/dmrprivacycodec.h"
    "${OBRII_ROOT}/src/utils/dmrprivacyutils.cpp"
    "${OBRII_ROOT}/src/utils/dmrprivacyutils.h"
    "${OBRII_ROOT}/src/digital/cwaudiodecoder.cpp"
    "${OBRII_ROOT}/src/digital/psk31audiodecoder.cpp"
    "${OBRII_ROOT}/src/audio/audioprocessor.cpp"
    "${OBRII_ROOT}/src/audio/audiofilterchain.cpp"
    "${OBRII_ROOT}/src/core/iqbuffer.cpp"
    "${OBRII_ROOT}/src/core/iqchannelizer.cpp"
    "${OBRII_ROOT}/src/core/channelizerutils.cpp"
    "${OBRII_ROOT}/third_party/tiny_aes/aes.c")
target_include_directories(ObriiCliModule PRIVATE
    "${OBRII_ROOT}/src/cli" "${OBRII_ROOT}/src/audio"
    "${OBRII_ROOT}/src/core" "${OBRII_ROOT}/src/digital"
    "${OBRII_ROOT}/src/utils" "${OBRII_ROOT}/src/backends"
    "${OBRII_ROOT}/third_party/tiny_aes"
    "${OBRII_ROOT}/FobosDMRVoiceBackend-gpl/include" "${FFTW_INCLUDE_DIR}")
target_link_libraries(ObriiCliModule PRIVATE Qt5::Core "${FFTW_FLOAT_LIBRARY}")
target_compile_definitions(ObriiCliModule PRIVATE AES256=1 CBC=0 CTR=0 ECB=1)
if(WIN32)
    target_link_libraries(ObriiCliModule PRIVATE winmm)
else()
    target_link_libraries(ObriiCliModule PRIVATE Threads::Threads)
    if(Qt5Multimedia_FOUND)
        target_link_libraries(ObriiCliModule PRIVATE Qt5::Multimedia)
        target_compile_definitions(ObriiCliModule PRIVATE FOBOSAPP_HAS_QT_AUDIO=1)
    endif()
endif()
if(MSVC)
    target_compile_options(ObriiCliModule PRIVATE /utf-8)
endif()

add_executable(ObriiCliPacketModule
    obriiclipacketmodule.cpp climoduleprotocol.h
    "${OBRII_ROOT}/src/openradio/ax25livedecoder.cpp"
    "${OBRII_ROOT}/src/openradio/ax25afsk1200.cpp"
    "${OBRII_ROOT}/src/openradio/ax25codec.cpp"
    "${OBRII_ROOT}/src/openradio/kisscodec.cpp")
target_include_directories(ObriiCliPacketModule PRIVATE
    "${OBRII_ROOT}/src/cli" "${OBRII_ROOT}/src/openradio")
target_link_libraries(ObriiCliPacketModule PRIVATE Qt5::Core Qt5::Network)
if(MSVC)
    target_compile_options(ObriiCliPacketModule PRIVATE /utf-8)
endif()

add_executable(ObriiCliImageModule
    obriicliimagemodule.cpp climoduleprotocol.h
    "${OBRII_ROOT}/src/digital/videoprocessor.cpp")
target_include_directories(ObriiCliImageModule PRIVATE
    "${OBRII_ROOT}/src/cli" "${OBRII_ROOT}/src/digital"
    "${OBRII_ROOT}/src/core")
target_link_libraries(ObriiCliImageModule PRIVATE Qt5::Core Qt5::Gui)
if(MSVC)
    target_compile_options(ObriiCliImageModule PRIVATE /utf-8)
endif()

add_executable(ObriiCliSpectrumModule
    obriiclispectrummodule.cpp climoduleprotocol.h)
target_include_directories(ObriiCliSpectrumModule PRIVATE "${OBRII_ROOT}/src/cli")
target_link_libraries(ObriiCliSpectrumModule PRIVATE Qt5::Core)
if(MSVC)
    target_compile_options(ObriiCliSpectrumModule PRIVATE /utf-8)
endif()

set(OBRII_M17_SOURCES
    "${OBRII_ROOT}/third_party/libm17/encode/convol.c"
    "${OBRII_ROOT}/third_party/libm17/encode/symbols.c"
    "${OBRII_ROOT}/third_party/libm17/decode/viterbi.c"
    "${OBRII_ROOT}/third_party/libm17/decode/symbols.c"
    "${OBRII_ROOT}/third_party/libm17/phy/sync.c"
    "${OBRII_ROOT}/third_party/libm17/phy/slice.c"
    "${OBRII_ROOT}/third_party/libm17/phy/interleave.c"
    "${OBRII_ROOT}/third_party/libm17/phy/randomize.c"
    "${OBRII_ROOT}/third_party/libm17/math/rrc.c"
    "${OBRII_ROOT}/third_party/libm17/math/math.c"
    "${OBRII_ROOT}/third_party/libm17/math/golay.c"
    "${OBRII_ROOT}/third_party/libm17/payload/lich.c"
    "${OBRII_ROOT}/third_party/libm17/payload/call.c"
    "${OBRII_ROOT}/third_party/libm17/payload/crc.c"
    "${OBRII_ROOT}/third_party/libm17/payload/lsf.c"
    "${OBRII_ROOT}/third_party/libm17/m17.c")
add_library(ObriiCliM17Core STATIC ${OBRII_M17_SOURCES})
set_target_properties(ObriiCliM17Core PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(ObriiCliM17Core PUBLIC "${OBRII_ROOT}/third_party/libm17")
if(UNIX)
    target_link_libraries(ObriiCliM17Core PUBLIC m)
endif()
add_executable(ObriiCliM17Module
    obriiclim17module.cpp
    "${OBRII_ROOT}/src/openradio/m17adapter.cpp")
target_include_directories(ObriiCliM17Module PRIVATE
    "${OBRII_ROOT}/src/openradio" "${OBRII_ROOT}/third_party/libm17")
target_link_libraries(ObriiCliM17Module PRIVATE Qt5::Core ObriiCliM17Core)
target_sources(ObriiCliModule PRIVATE
    "${OBRII_ROOT}/src/digital/ft4audiodecoder.cpp"
    "${OBRII_ROOT}/src/digital/m17audiodecoder.cpp"
    "${OBRII_ROOT}/src/core/fftplannermutex.cpp"
    "${OBRII_ROOT}/third_party/ft8_lib/ft8/decode.c"
    "${OBRII_ROOT}/third_party/ft8_lib/ft8/ldpc.c"
    "${OBRII_ROOT}/third_party/ft8_lib/ft8/constants.c"
    "${OBRII_ROOT}/third_party/ft8_lib/ft8/crc.c"
    "${OBRII_ROOT}/third_party/ft8_lib/ft8/message.c"
    "${OBRII_ROOT}/third_party/ft8_lib/ft8/text.c")
target_include_directories(ObriiCliModule PRIVATE "${OBRII_ROOT}/third_party/ft8_lib")
target_link_libraries(ObriiCliModule PRIVATE ObriiCliM17Core)
target_compile_definitions(ObriiCliModule PRIVATE OBRII_HAS_EXTRA_RECEIVE_MODES=1)
if(MSVC)
    target_compile_options(ObriiCliM17Module PRIVATE /utf-8)
endif()

add_executable(ObriiCliTxModule
    clilivetransmit.cpp clilivetransmit.h climicrophonecapture.cpp climicrophonecapture.h
    "${OBRII_ROOT}/src/openradio/kissvirtualtx.cpp" "${OBRII_ROOT}/src/openradio/kissvirtualtx.h"
    "${OBRII_ROOT}/src/openradio/kisscodec.cpp" "${OBRII_ROOT}/src/openradio/kisscodec.h"
    "${OBRII_ROOT}/src/transmit/windowsmicrophonecapture.cpp"
    "${OBRII_ROOT}/src/transmit/windowsmicrophonecapture.h"
    obriiclitxmodule.cpp
    "${OBRII_ROOT}/src/openradio/m17adapter.cpp"
    "${OBRII_ROOT}/src/transmit/transmitmediagenerator.cpp"
    "${OBRII_ROOT}/src/transmit/transmitmediagenerator.h"
    "${OBRII_ROOT}/src/openradio/ax25afsk1200.cpp"
    "${OBRII_ROOT}/src/openradio/ax25codec.cpp"
    "${OBRII_ROOT}/src/transmit/signallabgenerator.cpp"
    "${OBRII_ROOT}/src/transmit/dmrtransmitencoder.cpp"
    "${OBRII_ROOT}/src/digital/dmrvocoder.cpp"
    "${OBRII_ROOT}/src/digital/dmrprivacycodec.cpp"
    "${OBRII_ROOT}/src/utils/dmrprivacyutils.cpp"
    "${OBRII_ROOT}/third_party/tiny_aes/aes.c"
    "${OBRII_ROOT}/src/transmit/transmitterbackend.cpp"
    "${OBRII_ROOT}/src/transmit/hackrftransmitterbackend.cpp"
    "${OBRII_ROOT}/src/backends/hackrfbackend.cpp"
    "${OBRII_ROOT}/src/backends/uhdbackend.cpp"
    "${OBRII_ROOT}/src/transmit/virtualtransmitterbackend.cpp"
    "${OBRII_ROOT}/src/transmit/transmitwaveformgenerator.cpp"
    "${OBRII_ROOT}/src/openradio/virtualsignalframe.cpp"
    "${OBRII_ROOT}/src/openradio/virtualsignaltransport.cpp"
    "${OBRII_ROOT}/third_party/ft8_lib/ft8/constants.c"
    "${OBRII_ROOT}/third_party/ft8_lib/ft8/crc.c"
    "${OBRII_ROOT}/third_party/ft8_lib/ft8/encode.c"
    "${OBRII_ROOT}/third_party/ft8_lib/ft8/message.c"
    "${OBRII_ROOT}/third_party/ft8_lib/ft8/text.c")
target_include_directories(ObriiCliTxModule PRIVATE
    "${OBRII_ROOT}/src/transmit" "${OBRII_ROOT}/src/openradio"
    "${OBRII_ROOT}/src/core" "${OBRII_ROOT}/src" "${OBRII_ROOT}/src/utils" "${OBRII_ROOT}/src/backends"
    "${OBRII_ROOT}/third_party/ft8_lib"
    "${OBRII_ROOT}/third_party/tiny_aes"
    "${FFTW_INCLUDE_DIR}"
    "${OBRII_ROOT}/FobosDMRVoiceBackend-gpl/include")
target_link_libraries(ObriiCliTxModule PRIVATE Qt5::Core Qt5::Gui Qt5::Network Qt5::Concurrent ObriiCliM17Core "${FFTW_FLOAT_LIBRARY}")
target_compile_definitions(ObriiCliTxModule PRIVATE AES256=1 CBC=0 CTR=0 ECB=1)
if(WIN32)
    target_link_libraries(ObriiCliTxModule PRIVATE winmm ole32)
else()
    target_link_libraries(ObriiCliTxModule PRIVATE Threads::Threads)
endif()
if(Qt5Multimedia_FOUND)
    target_link_libraries(ObriiCliTxModule PRIVATE Qt5::Multimedia)
    target_compile_definitions(ObriiCliTxModule PRIVATE OBRII_CLI_QT_MICROPHONE=1)
endif()
if(MSVC)
    target_compile_options(ObriiCliTxModule PRIVATE /utf-8)
endif()

if(Qt5Widgets_FOUND)
    add_executable(ObriiCliView obriicliview.cpp
        "${OBRII_ROOT}/src/core/fftplannermutex.cpp"
        clispectrummeter.cpp clispectrummeter.h
        clinetworkbridge.cpp clinetworkbridge.h
        "${OBRII_ROOT}/src/network/networkcontroller.cpp"
        "${OBRII_ROOT}/src/network/networkcontroller.h"
        "${OBRII_ROOT}/src/network/displayspectrumcodec.cpp"
        "${OBRII_ROOT}/src/network/displayspectrumcodec.h"
        "${OBRII_ROOT}/src/core/iqchannelizer.cpp"
        "${OBRII_ROOT}/src/core/iqchannelizer.h"
        "${OBRII_ROOT}/src/core/channelizerutils.cpp"
        "${OBRII_ROOT}/src/core/channelizerutils.h")
    target_include_directories(ObriiCliView PRIVATE
        "${OBRII_ROOT}/src/network" "${OBRII_ROOT}/src/core" "${FFTW_INCLUDE_DIR}")
    target_link_libraries(ObriiCliView PRIVATE Qt5::Core Qt5::Network Qt5::Widgets "${FFTW_FLOAT_LIBRARY}")
    if(MSVC)
        target_compile_options(ObriiCliView PRIVATE /utf-8)
    endif()
endif()

add_executable(ObriiCliGnssModule obriiclignssmodule.cpp
    "${OBRII_ROOT}/src/gnss/gnsssdrstream.cpp" "${OBRII_ROOT}/src/gnss/gnsssdrstream.h"
    "${OBRII_ROOT}/src/audio/playbackmanager.cpp" "${OBRII_ROOT}/src/audio/playbackmanager.h"
    "${OBRII_ROOT}/src/core/iqbuffer.cpp")
target_include_directories(ObriiCliGnssModule PRIVATE "${OBRII_ROOT}/src/gnss" "${OBRII_ROOT}/src/audio" "${OBRII_ROOT}/src/core")
target_link_libraries(ObriiCliGnssModule PRIVATE Qt5::Core Qt5::Network)
if(NOT WIN32)
    target_link_libraries(ObriiCliGnssModule PRIVATE Threads::Threads)
endif()
if(MSVC)
    target_compile_options(ObriiCliGnssModule PRIVATE /utf-8)
endif()

add_library(ObriiFftTransformCore STATIC "${OBRII_ROOT}/src/core/ffttransformcore.cpp" "${OBRII_ROOT}/src/core/fftplanprofile.cpp")
if(WIN32)
    target_compile_definitions(ObriiFftTransformCore PRIVATE FOBOSAPP_HAS_FFTW_THREADS=1)
else()
    find_library(FFTW_TUNING_THREADS NAMES fftw3f_threads HINTS "${FFTW_ROOT}/lib")
    if(FFTW_TUNING_THREADS)
        target_compile_definitions(ObriiFftTransformCore PRIVATE FOBOSAPP_HAS_FFTW_THREADS=1)
        target_link_libraries(ObriiFftTransformCore PUBLIC ${FFTW_TUNING_THREADS})
    endif()
endif()
target_include_directories(ObriiFftTransformCore PUBLIC "${OBRII_ROOT}/src/core" "${FFTW_INCLUDE_DIR}")
target_link_libraries(ObriiFftTransformCore PUBLIC Qt5::Core "${FFTW_FLOAT_LIBRARY}")
add_library(ObriiRecordingFormatCore STATIC "${OBRII_ROOT}/src/core/recordingformatcore.cpp")
set_target_properties(ObriiFftTransformCore ObriiRecordingFormatCore PROPERTIES AUTOMOC OFF)
target_include_directories(ObriiRecordingFormatCore PUBLIC "${OBRII_ROOT}/src/core")
target_link_libraries(ObriiRecordingFormatCore PUBLIC Qt5::Core)
target_link_libraries(ObriiCli PRIVATE ObriiFftTransformCore ObriiRecordingFormatCore ObriiCommandNetwork ObriiUsrpLabCore)
if(TARGET ObriiCliView)
    target_link_libraries(ObriiCliView PRIVATE ObriiFftTransformCore)
endif()

# The standalone CLI uses the same optional node service as the desktop build.
if(NOT TARGET ObriiMeshNode)
    set(OBRII_MESH_BUILD_UI OFF)
    add_subdirectory("${OBRII_ROOT}/src/mesh" "${CMAKE_CURRENT_BINARY_DIR}/mesh" EXCLUDE_FROM_ALL)
endif()
target_link_libraries(ObriiCli PRIVATE ObriiMeshNode)
