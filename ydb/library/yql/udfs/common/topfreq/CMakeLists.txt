
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(topfreq_udf INTERFACE)
target_link_libraries(topfreq_udf INTERFACE
  contrib-libs-cxxsupp
  yutil
  yql-public-udf
  public-udf-support
  common-topfreq-static
)

add_global_library_for(topfreq_udf.global topfreq_udf)
target_compile_options(topfreq_udf.global PRIVATE
  -DUDF_ABI_VERSION_MAJOR=2
  -DUDF_ABI_VERSION_MINOR=10
  -DUDF_ABI_VERSION_PATCH=0
)
target_link_libraries(topfreq_udf.global PUBLIC
  contrib-libs-cxxsupp
  yutil
  yql-public-udf
  public-udf-support
  common-topfreq-static
)
target_sources(topfreq_udf.global PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/udfs/common/topfreq/topfreq_udf.cpp
)
