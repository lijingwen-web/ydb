
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(yq-libs-audit)
target_link_libraries(yq-libs-audit PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-actors-core
)
target_sources(yq-libs-audit PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/core/yq/libs/audit/yq_audit_service.cpp
)
