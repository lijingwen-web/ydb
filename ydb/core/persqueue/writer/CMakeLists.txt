
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(core-persqueue-writer)
target_link_libraries(core-persqueue-writer PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-actors-core
  cpp-string_utils-base64
  ydb-core-base
  core-persqueue-events
  ydb-core-protos
  public-lib-base
)
target_sources(core-persqueue-writer PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/core/persqueue/writer/source_id_encoding.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/persqueue/writer/writer.cpp
)
