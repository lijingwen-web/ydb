
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(restricted-boost-container)
target_compile_options(restricted-boost-container PRIVATE
  -Wno-everything
)
target_include_directories(restricted-boost-container PUBLIC
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/container/include
)
target_link_libraries(restricted-boost-container PUBLIC
  contrib-libs-cxxsupp
  yutil
  restricted-boost-assert
  restricted-boost-config
  restricted-boost-container_hash
  restricted-boost-core
  restricted-boost-intrusive
  restricted-boost-move
  restricted-boost-static_assert
  restricted-boost-type_traits
)
target_sources(restricted-boost-container PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/container/src/alloc_lib.c
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/container/src/dlmalloc.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/container/src/global_resource.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/container/src/monotonic_buffer_resource.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/container/src/pool_resource.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/container/src/synchronized_pool_resource.cpp
  ${CMAKE_SOURCE_DIR}/contrib/restricted/boost/container/src/unsynchronized_pool_resource.cpp
)
