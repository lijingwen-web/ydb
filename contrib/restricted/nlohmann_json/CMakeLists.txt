
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(contrib-restricted-nlohmann_json INTERFACE)
target_include_directories(contrib-restricted-nlohmann_json INTERFACE
  ${CMAKE_SOURCE_DIR}/contrib/restricted/nlohmann_json/include
)
target_link_libraries(contrib-restricted-nlohmann_json INTERFACE
  contrib-libs-cxxsupp
)
