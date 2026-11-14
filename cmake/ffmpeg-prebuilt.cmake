# Prebuilt FFmpeg for the desktop build: BtbN's LGPL shared build, pinned to a month-end autobuild
# (those releases are kept long-term). Building FFmpeg from source through vcpkg took far too long.
# The DLLs are plain C ABI, so the MinGW-built binaries link fine against MSVC through the .lib
# import libraries in the archive.
set(SACRIFICE_FFMPEG_NAME "ffmpeg-n8.1.3-27-g8dca1586f4-win64-lgpl-shared-8.1")
set(SACRIFICE_FFMPEG_URL
  "https://github.com/FFmpeg-Builds/FFmpeg-Builds/releases/download/autobuild-2026-10-31-12-59/${SACRIFICE_FFMPEG_NAME}.zip")
set(SACRIFICE_FFMPEG_SHA256 "5cef7bc48e513f3cee7c881e8b2999d835541edb1846c3e89443a67f8f8556ab")

set(SACRIFICE_FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/out/ffmpeg/${SACRIFICE_FFMPEG_NAME}")
if(NOT EXISTS "${SACRIFICE_FFMPEG_ROOT}/lib/avcodec.lib")
  set(archive "${CMAKE_CURRENT_SOURCE_DIR}/out/downloads/${SACRIFICE_FFMPEG_NAME}.zip")
  if(NOT EXISTS "${archive}")
    message(STATUS "Downloading prebuilt FFmpeg (${SACRIFICE_FFMPEG_NAME})")
  endif()
  # Re-checks the hash of an already downloaded archive and fetches again if it does not match.
  file(DOWNLOAD "${SACRIFICE_FFMPEG_URL}" "${archive}" EXPECTED_HASH SHA256=${SACRIFICE_FFMPEG_SHA256}
    SHOW_PROGRESS STATUS status)
  list(GET status 0 code)
  if(code)
    list(GET status 1 reason)
    message(FATAL_ERROR "Cannot download FFmpeg from ${SACRIFICE_FFMPEG_URL}: ${reason}")
  endif()
  file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/out/ffmpeg")
  unset(archive)
endif()

set(FFMPEG_INCLUDE_DIRS "${SACRIFICE_FFMPEG_ROOT}/include")
set(FFMPEG_LIBRARY_DIRS "${SACRIFICE_FFMPEG_ROOT}/lib")
set(FFMPEG_LIBRARIES avcodec avformat avutil swresample swscale)
# Only the libraries the engine links; avfilter, avdevice and the command-line tools are left out.
file(GLOB SACRIFICE_FFMPEG_DLLS
  "${SACRIFICE_FFMPEG_ROOT}/bin/avcodec-*.dll" "${SACRIFICE_FFMPEG_ROOT}/bin/avformat-*.dll"
  "${SACRIFICE_FFMPEG_ROOT}/bin/avutil-*.dll" "${SACRIFICE_FFMPEG_ROOT}/bin/swresample-*.dll"
  "${SACRIFICE_FFMPEG_ROOT}/bin/swscale-*.dll")
# Every executable lands in the build directory root; put the DLLs beside them so the game and the
# tests start without an install step.
file(COPY ${SACRIFICE_FFMPEG_DLLS} DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
