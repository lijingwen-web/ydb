
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(util-charset)
target_link_libraries(util-charset PUBLIC
  contrib-libs-cxxsupp
)
target_sources(util-charset PRIVATE
  ${CMAKE_SOURCE_DIR}/util/charset/wide_sse41.cpp
)
set_property(
  SOURCE
  ${CMAKE_SOURCE_DIR}/util/charset/wide_sse41.cpp
  APPEND
  PROPERTY
  COMPILE_OPTIONS
  -msse4.1
)
target_joined_source(util-charset
  all_charset.cpp
  ${CMAKE_SOURCE_DIR}/util/charset/generated/unidata.cpp
  ${CMAKE_SOURCE_DIR}/util/charset/recode_result.cpp
  ${CMAKE_SOURCE_DIR}/util/charset/unicode_table.cpp
  ${CMAKE_SOURCE_DIR}/util/charset/unidata.cpp
  ${CMAKE_SOURCE_DIR}/util/charset/utf8.cpp
  ${CMAKE_SOURCE_DIR}/util/charset/wide.cpp
)
