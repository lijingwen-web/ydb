
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(library-cpp-balloc)
target_compile_options(library-cpp-balloc PRIVATE
  -Wno-everything
)
target_link_libraries(library-cpp-balloc PUBLIC
  contrib-libs-cxxsupp
  cpp-balloc-lib
)
target_sources(library-cpp-balloc PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/balloc/balloc.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/balloc/malloc-info.cpp
)
