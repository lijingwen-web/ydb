
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(providers-common-http_gateway)
target_compile_options(providers-common-http_gateway PRIVATE
  -DUSE_CURRENT_UDF_ABI_VERSION
)
target_link_libraries(providers-common-http_gateway PUBLIC
  contrib-libs-cxxsupp
  yutil
  contrib-libs-curl
  cpp-monlib-dynamic_counters
  library-cpp-retry
  providers-common-proto
  yql-public-issue
)
target_sources(providers-common-http_gateway PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/common/http_gateway/yql_http_gateway.cpp
)
