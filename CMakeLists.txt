cmake_minimum_required(VERSION 3.10 FATAL_ERROR)

project(TelegramBotApi VERSION 10.3 LANGUAGES CXX)

option(USE_PREBUILT_TDLIB "Use precompiled TDLib static libraries" ON)
set(PREBUILT_TDLIB_DIR "" CACHE PATH "Path to precompiled TDLib directory")

if (NOT USE_PREBUILT_TDLIB OR NOT EXISTS "${PREBUILT_TDLIB_DIR}/include/td/actor/actor.h")
  message(FATAL_ERROR "Precompiled TDLib is required! Please provide a valid prebuilt directory via -DUSE_PREBUILT_TDLIB=ON -DPREBUILT_TDLIB_DIR=<path>")
endif()

if (NOT DEFINED CMAKE_MODULE_PATH)
  set(CMAKE_MODULE_PATH "")
endif()
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake" "${CMAKE_MODULE_PATH}")

if (TARGET tdnet)
  include(CMake/RawMcpJson.cmake)
endif()

if (NOT DEFINED CMAKE_INSTALL_BINDIR)
  set(CMAKE_INSTALL_BINDIR "bin")
endif()

if (POLICY CMP0074)
  # use environment variables to find libraries
  cmake_policy(SET CMP0074 NEW)
endif()

include(PreventInSourceBuild)
prevent_in_source_build()

if (TELEGRAM_BOT_API_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT IPO_SUPPORTED LANGUAGES CXX)
  if (IPO_SUPPORTED)
    string(REPLACE ";" " " CXX_FLAGS_IPO "${CMAKE_CXX_COMPILE_OPTIONS_IPO}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CXX_FLAGS_IPO}")

    string(REPLACE ";" " " LINK_FLAGS_IPO "${CMAKE_CXX_LINK_OPTIONS_IPO}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LINK_FLAGS_IPO}")
  endif()
endif()

set(CMAKE_THREAD_PREFER_PTHREAD ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if (THREADS_HAVE_PTHREAD_ARG)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

include(TdSetUpCompiler)
td_set_up_compiler()

if (CLANG OR GCC)
  if (MEMPROF)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-no-pie CXX_NO_PIE_FLAG)
    if (CXX_NO_PIE_FLAG)
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -no-pie")
    elseif (APPLE)
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-no_pie")
    endif()
    include(AddCXXCompilerFlag)
    add_cxx_compiler_flag("-static-libstdc++")
    add_cxx_compiler_flag("-static-libgcc")
  endif()
endif()

set(TELEGRAM_BOT_API_SOURCE
  telegram-bot-api/telegram-bot-api.cpp

  telegram-bot-api/Client.cpp
  telegram-bot-api/ClientManager.cpp
  telegram-bot-api/HttpConnection.cpp
  telegram-bot-api/Query.cpp
  telegram-bot-api/Stats.cpp
  telegram-bot-api/Watchdog.cpp
  telegram-bot-api/WebhookActor.cpp

  telegram-bot-api/Client.h
  telegram-bot-api/ClientManager.h
  telegram-bot-api/ClientParameters.h
  telegram-bot-api/HttpConnection.h
  telegram-bot-api/HttpServer.h
  telegram-bot-api/Query.h
  telegram-bot-api/Stats.h
  telegram-bot-api/Watchdog.h
  telegram-bot-api/WebhookActor.h
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/telegram-bot-api/stats.html" STATS_HTML_CONTENT)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/telegram-bot-api/stats_html.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/stats_html.h"
  @ONLY
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/telegram-bot-api/stats.html")

add_executable(telegram-bot-api ${TELEGRAM_BOT_API_SOURCE})
message(STATUS "Using precompiled TDLib from: ${PREBUILT_TDLIB_DIR}")
target_include_directories(telegram-bot-api PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${PREBUILT_TDLIB_DIR}/include"
)

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

file(GLOB PREBUILT_TDLIB_LIBS "${PREBUILT_TDLIB_DIR}/lib/*.a")
if (NOT PREBUILT_TDLIB_LIBS)
  message(FATAL_ERROR "No static libraries found in ${PREBUILT_TDLIB_DIR}/lib")
endif()

target_link_libraries(telegram-bot-api PRIVATE
  -Wl,--start-group
  ${PREBUILT_TDLIB_LIBS}
  -Wl,--end-group
  OpenSSL::Crypto
  OpenSSL::SSL
  ZLIB::ZLIB
  Threads::Threads
  ${CMAKE_DL_LIBS}
)

install(TARGETS telegram-bot-api RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if (MSVC AND VCPKG_TOOLCHAIN)
  install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/" DESTINATION "${CMAKE_INSTALL_BINDIR}" FILES_MATCHING PATTERN "*.dll" PATTERN "*.pdb")
endif()
