cmake_minimum_required(VERSION 3.28.0)
project(DSHM C CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# DSHM
add_library(dshm INTERFACE)
target_include_directories(dshm INTERFACE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dshm INTERFACE atomic)

# Build Python
find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
set(VENV_DIR ${CMAKE_SOURCE_DIR}/.venv)
set(VENV_PYTHON ${VENV_DIR}/bin/python)

if(NOT EXISTS ${VENV_PYTHON})
    message(STATUS "Creating project virtual environment at ${VENV_DIR}")

    execute_process(
        COMMAND ${Python3_EXECUTABLE} -m venv ${VENV_DIR}
        RESULT_VARIABLE VENV_RESULT
    )

    if(VENV_RESULT)
        message(FATAL_ERROR "Failed to create Python virtual environment")
    endif()

    execute_process(
        COMMAND ${VENV_PYTHON} -m pip install --upgrade pip pybind11-stubgen pytest
        RESULT_VARIABLE PIP_RESULT
    )

    if(PIP_RESULT)
        message(FATAL_ERROR "Failed to install pybind11-stubgen")
    endif()
endif()

# Avoid installing third-party headers/config into the wheel.
set(PYBIND11_INSTALL OFF)
set(PYBIND11_TEST OFF)
add_subdirectory(extern/pybind11)
pybind11_add_module(dshmpy src/module.cpp)
target_link_libraries(dshmpy PRIVATE dshm)

set(DSHMPY_STUB_DIR "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
if(NOT DSHMPY_STUB_DIR)
    set(DSHMPY_STUB_DIR "${CMAKE_CURRENT_BINARY_DIR}/lib")
endif()

add_custom_command(
    TARGET dshmpy
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E env
        PYTHONPATH=$<TARGET_FILE_DIR:dshmpy>
        ${VENV_PYTHON} -m pybind11_stubgen dshmpy
        --output-dir ${DSHMPY_STUB_DIR}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# Tests
set(DOCTEST_NO_INSTALL ON)
add_subdirectory(extern/doctest)

set(DSHM_CPP_TEST_SOURCE
    ${CMAKE_SOURCE_DIR}/tests/cpp/test_dshm_main.cpp
    ${CMAKE_SOURCE_DIR}/tests/cpp/test_shared_heap.cpp
    ${CMAKE_SOURCE_DIR}/tests/cpp/test_shared_object.cpp
)

add_executable(dshm_test ${DSHM_CPP_TEST_SOURCE})
target_link_libraries(dshm_test PRIVATE dshm doctest::doctest)

install(TARGETS dshmpy
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .
)

install(FILES ${DSHMPY_STUB_DIR}/dshmpy.pyi
    DESTINATION .
)