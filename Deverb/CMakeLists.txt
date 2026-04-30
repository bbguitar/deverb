cmake_minimum_required(VERSION 3.25)
project(Deverb VERSION 1.0.0)

set(IPLUG2_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../iPlug2 CACHE PATH "iPlug2 root directory")
include(${IPLUG2_DIR}/iPlug2.cmake)

find_package(iPlug2 REQUIRED)

find_library(FFTW3F_LIB NAMES fftw3f
  PATHS /usr/local/lib /opt/homebrew/lib
  REQUIRED)
find_path(FFTW3_INCLUDE_DIR NAMES fftw3.h
  PATHS /usr/local/include /opt/homebrew/include
  REQUIRED)

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/postfish-src
  ${FFTW3_INCLUDE_DIR}
)

iplug_add_plugin(${PROJECT_NAME}
  SOURCES
    DeverbPlugin.h
    Deverb.cpp
    DeverbDSP.h
    DeverbDSP.cpp
    postfish-src/subband.c
    postfish-src/bessel.c
    postfish-src/lpc.c
    postfish-src/window.c
    postfish-src/reconstruct.c
    postfish-src/feedback.c
    resources/resource.h
  RESOURCES
    resources/fonts/Roboto-Regular.ttf
  FORMATS APP VST3 AU
  UI IGRAPHICS
  LINK ${FFTW3F_LIB}
)
