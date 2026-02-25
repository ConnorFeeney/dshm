cmake_minimum_required(VERSION 3.28.0)
project(DSHM C CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE DSHM_SOURCE CONFIGURE_DEPENDS "*.cpp" "*.h" "*.hpp")
list(FILTER DSHM_SOURCE EXCLUDE REGEX "/extern/pybind11/tests/")
list(FILTER DSHM_SOURCE EXCLUDE REGEX "/extern/pybind11/docs/")

add_library(dshm ${DSHM_SOURCE})
target_include_directories(dshm PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_executable(dshm_test examples/shm_test.cc)
target_link_libraries(dshm_test PRIVATE dshm)
