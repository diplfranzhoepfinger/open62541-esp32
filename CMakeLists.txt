# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(SUPPORTED_TARGETS esp32)

include($ENV{IDF_PATH}/tools/cmakev2/idf.cmake)
project(open62541-esp32 C CXX ASM)
idf_project_default()