cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME HiradoraGen3FW)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# User-owned linker reserves the calibration page; CubeMX file remains untouched.
string(REPLACE "STM32G431XX_FLASH.ld" "STM32G431xx_CAL_FLASH.ld" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
string(REPLACE "STM32G431xx_FLASH.ld" "STM32G431xx_CAL_FLASH.ld" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

set_property(TARGET ${CMAKE_PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/STM32G431xx_CAL_FLASH.ld")

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Drivers/CMSIS/DSP/Source/ControllerFunctions/arm_sin_cos_f32.c
    Drivers/CMSIS/DSP/Source/CommonTables/arm_common_tables.c
    Core/Src/irq_trace.c
    Core/Src/foc_voltage.c
    Core/Src/voltage_vector.c
    Core/Src/calibration_store.c
    Core/Src/motor_calibration.c
    Core/Src/bus_voltage.c
    Core/Src/as5047p.c
    Core/Src/console.c
    Core/Src/current_sense.c
    Core/Src/dma_logger.c
    Core/Src/motor_control.c
    Core/Src/ntc.c
    Core/Src/stspin32g4.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Drivers/CMSIS/DSP/Include
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Enable floating-point conversions such as %f in newlib-nano printf.
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -u _printf_float
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# 20 kHz周期処理はサイズ優先(-Os)より実行時間を優先する。
# Debugでも同じ演算経路を使うため、これらの周期処理ファイルは-O2でビルドする。
set_source_files_properties(
    Core/Src/foc_voltage.c Core/Src/voltage_vector.c
    Core/Src/as5047p.c Core/Src/current_sense.c Core/Src/motor_control.c
    Drivers/CMSIS/DSP/Source/ControllerFunctions/arm_sin_cos_f32.c
    PROPERTIES COMPILE_OPTIONS "-O2")

# 計測フックの負荷をDebugでも小さく保つ。
set_source_files_properties(Core/Src/irq_trace.c PROPERTIES COMPILE_OPTIONS "-O2")
