
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(providers-s3-path_generator)
target_link_libraries(providers-s3-path_generator PUBLIC
  contrib-libs-cxxsupp
  yutil
  library-cpp-scheme
  yql-public-udf
  tools-enum_parser-enum_serialization_runtime
)
target_sources(providers-s3-path_generator PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/s3/path_generator/yql_s3_path_generator.cpp
)
generate_enum_serilization(providers-s3-path_generator
  ${CMAKE_SOURCE_DIR}/ydb/library/yql/providers/s3/path_generator/yql_s3_path_generator.h
  INCLUDE_HEADERS
  ydb/library/yql/providers/s3/path_generator/yql_s3_path_generator.h
)
