
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.



add_library(lib-MC-MCParser)
target_compile_options(lib-MC-MCParser PRIVATE
  -Wno-everything
)
target_include_directories(lib-MC-MCParser PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/MC/MCParser
)
target_link_libraries(lib-MC-MCParser PUBLIC
  contrib-libs-cxxsupp
  contrib-libs-llvm12
  llvm12-lib-MC
  llvm12-lib-Support
)
target_sources(lib-MC-MCParser PRIVATE
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/MC/MCParser/AsmLexer.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/MC/MCParser/AsmParser.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/MC/MCParser/COFFAsmParser.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/MC/MCParser/COFFMasmParser.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/MC/MCParser/DarwinAsmParser.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/MC/MCParser/ELFAsmParser.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/MC/MCParser/MCAsmLexer.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/MC/MCParser/MCAsmParser.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/MC/MCParser/MCAsmParserExtension.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/MC/MCParser/MCTargetAsmParser.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/MC/MCParser/MasmParser.cpp
  ${CMAKE_SOURCE_DIR}/contrib/libs/llvm12/lib/MC/MCParser/WasmAsmParser.cpp
)
