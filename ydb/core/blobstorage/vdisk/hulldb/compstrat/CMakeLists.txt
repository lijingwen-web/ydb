
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(vdisk-hulldb-compstrat)
target_link_libraries(vdisk-hulldb-compstrat PUBLIC
  contrib-libs-cxxsupp
  yutil
  vdisk-hulldb-base
  vdisk-hulldb-generic
  ydb-core-protos
)
target_sources(vdisk-hulldb-compstrat PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/core/blobstorage/vdisk/hulldb/compstrat/hulldb_compstrat_defs.cpp
  ${CMAKE_SOURCE_DIR}/ydb/core/blobstorage/vdisk/hulldb/compstrat/hulldb_compstrat_selector.cpp
)
