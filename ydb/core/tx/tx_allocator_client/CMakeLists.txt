
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(core-tx-tx_allocator_client)
target_link_libraries(core-tx-tx_allocator_client PUBLIC
  contrib-libs-cxxsupp
  yutil
  ydb-core-base
  ydb-core-protos
  ydb-core-tablet
  core-tx-tx_allocator
)
target_sources(core-tx-tx_allocator_client PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/core/tx/tx_allocator_client/actor_client.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/tx/tx_allocator_client/client.cpp
)
