
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(public-lib-idx_test)
target_link_libraries(public-lib-idx_test PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-string_utils-base64
  cpp-client-ydb_table
  tools-enum_parser-enum_serialization_runtime
)
target_sources(public-lib-idx_test PRIVATE
  ${CMAKE_SOURCE_DIR}/ydb/public/lib/idx_test/idx_test_checker.cpp
  ${CMAKE_SOURCE_DIR}/ydb/public/lib/idx_test/idx_test_common.cpp
  ${CMAKE_SOURCE_DIR}/ydb/public/lib/idx_test/idx_test_data_provider.cpp
  ${CMAKE_SOURCE_DIR}/ydb/public/lib/idx_test/idx_test_stderr_progress_tracker.cpp
  ${CMAKE_SOURCE_DIR}/ydb/public/lib/idx_test/idx_test_loader.cpp
  ${CMAKE_SOURCE_DIR}/ydb/public/lib/idx_test/idx_test_upload.cpp
)
generate_enum_serilization(public-lib-idx_test
  ${CMAKE_SOURCE_DIR}/ydb/public/lib/idx_test/idx_test.h
  INCLUDE_HEADERS
  ydb/public/lib/idx_test/idx_test.h
)
