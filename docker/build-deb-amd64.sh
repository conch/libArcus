#!/bin/sh
#
# This scripts uses CMake to build libArcus with shared libraries targeting
# Linux x64. It also creates a debian package with cpack. The contents of the
# package are installed under "/usr".
#

mkdir build
cd build
cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_PYTHON=ON \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_STATIC=OFF \
    ..
make
cpack \
    --config ../cmake/cpack_config_deb_amd64.cmake \
    -D CPACK_INSTALL_CMAKE_PROJECTS="$(pwd);libArcus;ALL;/"
