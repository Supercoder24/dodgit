cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES
    ${CMAKE_CURRENT_SOURCE_DIR}/../modules/zephyr-dw3000-decadriver
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# The pinned decadriver includes "version.h"; Zephyr 4.5 moved it under zephyr/.
# Add the generated header directory without modifying the driver submodule.
zephyr_include_directories(${ZEPHYR_BINARY_DIR}/include/generated/zephyr)
project(ball)

target_sources(app PRIVATE src/main.c)
