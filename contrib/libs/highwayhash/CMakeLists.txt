
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(contrib-libs-highwayhash)
target_compile_options(contrib-libs-highwayhash PRIVATE
  -Wno-everything
)
target_include_directories(contrib-libs-highwayhash PUBLIC
  ${CMAKE_SOURCE_DIR}/contrib/libs/highwayhash
)
target_link_libraries(contrib-libs-highwayhash PUBLIC
  contrib-libs-cxxsupp
  yutil
  highwayhash-arch-avx2
  highwayhash-arch-sse41
)
target_sources(contrib-libs-highwayhash PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/libs/highwayhash/highwayhash/arch_specific.cc
  ${CMAKE_SOURCE_DIR}/contrib/libs/highwayhash/highwayhash/instruction_sets.cc
  ${CMAKE_SOURCE_DIR}/contrib/libs/highwayhash/highwayhash/nanobenchmark.cc
  ${CMAKE_SOURCE_DIR}/contrib/libs/highwayhash/highwayhash/os_specific.cc
  ${CMAKE_SOURCE_DIR}/contrib/libs/highwayhash/highwayhash/sip_hash.cc
  ${CMAKE_SOURCE_DIR}/contrib/libs/highwayhash/highwayhash/scalar_sip_tree_hash.cc
  ${CMAKE_SOURCE_DIR}/contrib/libs/highwayhash/highwayhash/hh_portable.cc
  ${CMAKE_SOURCE_DIR}/contrib/libs/highwayhash/highwayhash/c_bindings.cc
)
