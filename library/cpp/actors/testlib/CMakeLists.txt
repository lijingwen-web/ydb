
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(cpp-actors-testlib)
target_link_libraries(cpp-actors-testlib PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-actors-core
  actors-interconnect-mock
  cpp-actors-protos
  library-cpp-random_provider
  library-cpp-time_provider
)
target_sources(cpp-actors-testlib PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/actors/testlib/test_runtime.cpp
)
