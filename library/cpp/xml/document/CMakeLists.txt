
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(cpp-xml-document)
target_link_libraries(cpp-xml-document PUBLIC
  contrib-libs-cxxsupp
  yutil
  cpp-xml-init
  contrib-libs-libxml
  cpp-string_utils-ztstrbuf
)
target_sources(cpp-xml-document PRIVATE
  ${CMAKE_SOURCE_DIR}/library/cpp/xml/document/xml-document.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/xml/document/xml-textreader.cpp
  ${CMAKE_SOURCE_DIR}/library/cpp/xml/document/xml-options.cpp
)
