
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(tx-datashard-testload)
target_link_libraries(tx-datashard-testload PUBLIC
  contrib-libs-cxxsupp
  yutil
  contrib-libs-protobuf
  monlib-dynamic_counters-percentile
  monlib-service-pages
  ydb-core-base
  ydb-core-control
  ydb-core-keyvalue
  public-lib-base
  public-lib-operation_id
  cpp-client-ydb_proto
)
target_sources(tx-datashard-testload PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/core/tx/datashard/testload/test_load_actor.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/tx/datashard/testload/test_load_upsert.cpp
)
