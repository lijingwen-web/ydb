
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


if (APPLE)
  include(CMakeLists.darwin.txt)
elseif (UNIX AND NOT APPLE)
  include(CMakeLists.linux.txt)
endif()
