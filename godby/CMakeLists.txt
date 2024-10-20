cc_library(
  SMALL_FOOTPRINT

  NAME
    godby
  SOURCES
    **.cc
  HEADERS
    include/**.h
  INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}/include
  OPTIONS
    -O3
    -Wall
)
