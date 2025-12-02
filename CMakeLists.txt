cmake_minimum_required(VERSION 3.12)
project(dp-alpha)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

add_executable(dp-alpha dp-alpha.cpp)
target_link_libraries(dp-alpha PRIVATE PkgConfig::LIBDRM)
