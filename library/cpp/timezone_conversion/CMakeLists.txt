
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(library-cpp-timezone_conversion)
target_link_libraries(library-cpp-timezone_conversion PUBLIC
  contrib-libs-cxxsupp
  yutil
  libs-cctz-tzdata
  util-draft
  tools-enum_parser-enum_serialization_runtime
)
target_sources(library-cpp-timezone_conversion PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/timezone_conversion/convert.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/timezone_conversion/civil.cpp
)
generate_enum_serilization(library-cpp-timezone_conversion
  ${CMAKE_SOURCE_DIR}/library/cpp/timezone_conversion/civil.h
  INCLUDE_HEADERS
  library/cpp/timezone_conversion/civil.h
)
