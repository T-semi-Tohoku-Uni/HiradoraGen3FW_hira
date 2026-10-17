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
option(FOC_USE_CORDIC "Use STM32 CORDIC for voltage-vector sin/cos" ON)
option(FOC_ANGLE_PREDICTION "Advance observer angle to ADC/PWM target times" ON)
option(CAL_ENABLE_TEST_COMMANDS "Include cal test and cal trig test diagnostics" OFF)
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# User-owned linker reserves the calibration page; CubeMX file remains untouched.
string(REPLACE "STM32G431XX_FLASH.ld" "STM32G431xx_CAL_FLASH.ld" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
string(REPLACE "STM32G431xx_FLASH.ld" "STM32G431xx_CAL_FLASH.ld" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE FOC_ANGLE_PREDICTION=$<BOOL:${FOC_ANGLE_PREDICTION}>)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CAL_ENABLE_TEST_COMMANDS=$<BOOL:${CAL_ENABLE_TEST_COMMANDS}>)

set_property(TARGET ${CMAKE_PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/STM32G431xx_CAL_FLASH.ld")

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/can_protocol.c
    Core/Src/can_telemetry.c
    Core/Src/can_safety.c
    Core/Src/can_comm.c
    Drivers/CMSIS/DSP/Source/ControllerFunctions/arm_sin_cos_f32.c
    Drivers/CMSIS/DSP/Source/CommonTables/arm_common_tables.c
    Core/Src/foc_voltage.c
    Core/Src/angle_predictor.c
    Core/Src/current_pi.c
    Core/Src/voltage_vector.c
    Core/Src/calibration_store.c
    Core/Src/h2_fit.c
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

if(CAL_ENABLE_TEST_COMMANDS)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/trig_benchmark.c)
endif()

# Add include paths
# CAN追加分はDebugでもサイズを抑え、校正Flash領域を維持する。
set_source_files_properties(Core/Src/can_comm.c Core/Src/can_protocol.c Core/Src/can_telemetry.c Core/Src/can_safety.c
    PROPERTIES COMPILE_OPTIONS "-Os")
# 初期化コードもサイズ優先。FOC演算/ADC/TIM/SPIの最適化設定は維持する。
set_source_files_properties(Core/Src/main.c Core/Src/stm32g4xx_hal_msp.c Core/Src/console.c
    PROPERTIES COMPILE_OPTIONS "-Os")
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_fdcan.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart.c
    TARGET_DIRECTORY STM32_Drivers
    PROPERTIES COMPILE_OPTIONS "-Os")

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Drivers/CMSIS/DSP/Include
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    FOC_USE_CORDIC=$<BOOL:${FOC_USE_CORDIC}>
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
    Core/Src/foc_voltage.c Core/Src/angle_predictor.c Core/Src/voltage_vector.c Core/Src/trig_benchmark.c Core/Src/current_pi.c
    Core/Src/as5047p.c Core/Src/current_sense.c Core/Src/motor_control.c
    Drivers/CMSIS/DSP/Source/ControllerFunctions/arm_sin_cos_f32.c
    PROPERTIES COMPILE_OPTIONS "-O2")

# SPI応答を次のTIM/ADC IRQより前に処理する余裕をDebugでも確保する。
# FOC本体だけでなくHALディスパッチとIRQ入口/出口も周期処理の時間に含まれる。
# CubeMX生成コード自体は変更しない。ReleaseでもHAL/IRQを速度優先にする。
# 対象ファイルのDebugステップ順は変わり得る。
# dma_loggerはADC内の間引きとmainの200Hz整形の両方で使い、IRQの余裕を左右する。
set_source_files_properties(Core/Src/stm32g4xx_it.c Core/Src/dma_logger.c
    PROPERTIES COMPILE_OPTIONS "-O2")
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim.c
    TARGET_DIRECTORY STM32_Drivers
    PROPERTIES COMPILE_OPTIONS "-O2")

# Keep Debug within the 126 KiB application region after angle prediction.
# Preserve the reserved calibration page; only these HAL units use -Os in Debug.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_spi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_gpio.c
    TARGET_DIRECTORY STM32_Drivers
    PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-Os>")
