cmake_minimum_required(VERSION 3.16)
project(ObriiCommandSelfTests LANGUAGES CXX)

get_filename_component(OBRII_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_package(Qt5 REQUIRED COMPONENTS Core Network Widgets)
find_package(Threads REQUIRED)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
enable_testing()

add_library(ObriiCommandCore STATIC
    "${OBRII_ROOT}/src/core/commanddispatcher.cpp")
target_include_directories(ObriiCommandCore PUBLIC "${OBRII_ROOT}/src/core")
target_link_libraries(ObriiCommandCore PUBLIC Qt5::Core)

add_library(ObriiCommandNetwork STATIC
    "${OBRII_ROOT}/src/network/commandcontrolserver.cpp"
    "${OBRII_ROOT}/src/network/commandcontrolserver.h")
target_include_directories(ObriiCommandNetwork PUBLIC "${OBRII_ROOT}/src/network")
target_link_libraries(ObriiCommandNetwork PUBLIC ObriiCommandCore Qt5::Network)

add_library(ObriiCommandUi STATIC
    "${OBRII_ROOT}/src/ui/commandconsoledialog.cpp"
    "${OBRII_ROOT}/src/ui/commandconsoledialog.h")
target_include_directories(ObriiCommandUi PUBLIC "${OBRII_ROOT}/src/ui")
target_link_libraries(ObriiCommandUi PUBLIC ObriiCommandNetwork Qt5::Widgets)

add_executable(ObriiCommandDispatcherSelfTest
    "${OBRII_ROOT}/tools/command_dispatcher_selftest.cpp")
target_link_libraries(ObriiCommandDispatcherSelfTest PRIVATE ObriiCommandCore Threads::Threads)
add_executable(ObriiCommandControlSelfTest
    "${OBRII_ROOT}/tools/command_control_selftest.cpp")
target_link_libraries(ObriiCommandControlSelfTest PRIVATE ObriiCommandNetwork)
add_executable(ObriiCommandConsoleSelfTest
    "${OBRII_ROOT}/tools/command_console_selftest.cpp")
target_link_libraries(ObriiCommandConsoleSelfTest PRIVATE ObriiCommandUi)

foreach(target ObriiCommandDispatcherSelfTest ObriiCommandControlSelfTest ObriiCommandConsoleSelfTest)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
    add_test(NAME ${target} COMMAND ${CMAKE_COMMAND} -E env
        "QT_QPA_PLATFORM=offscreen" $<TARGET_FILE:${target}>)
    set_tests_properties(${target} PROPERTIES TIMEOUT 120)
endforeach()
if(MSVC)
    target_compile_options(ObriiCommandUi PRIVATE /utf-8)
endif()
