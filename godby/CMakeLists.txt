CPMAddPackage("gh:TartanLlama/expected#v1.1.0")
if (expected_ADDED)
  install(TARGETS expected DESTINATION lib)
  include_directories(${expected_SOURCE_DIR}/include)
  install(DIRECTORY ${expected_SOURCE_DIR}/include DESTINATION ${CMAKE_INSTALL_PREFIX})
endif()

cc_library(
  NAME godby
  INCLUDES ${CMAKE_CURRENT_LIST_DIR}/include
  SOURCES **.cc
  HEADERS include/**.h
  OPTIONS -O3 -Wall
  DEPENDENCIES expected
)
