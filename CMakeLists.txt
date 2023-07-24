cmake_minimum_required(VERSION 3.17)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(FAMILY rp2040)
set(BOARD pico_sdk)
set(CMAKE_BUILD_TYPE Debug)
#set(CMAKE_CXX_FLAGS "-g -Wall -Wextra")

include(${CMAKE_CURRENT_SOURCE_DIR}/external/tinyusb/hw/bsp/family_support.cmake)

# gets PROJECT name for the example (e.g. <BOARD>-<DIR_NAME>)
family_get_project_name(PROJECT DrmDmp64_mass)

project(${PROJECT} C CXX ASM)

# Checks this example is valid for the family and initializes the project
family_initialize_project(${PROJECT} ${CMAKE_CURRENT_LIST_DIR})

# Espressif has its own cmake build system
if(FAMILY STREQUAL "espressif")
  return()
endif()

add_executable(${PROJECT})


add_custom_command(OUTPUT ${CMAKE_CURRENT_LIST_DIR}/src/generated/joybus.pio.h
        DEPENDS ${CMAKE_CURRENT_LIST_DIR}/src/joybus.pio
        COMMAND pioasm/pioasm ${CMAKE_CURRENT_LIST_DIR}/src/joybus.pio ${CMAKE_CURRENT_LIST_DIR}/src/generated/joybus.pio.h
        )

target_sources(${PROJECT} PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/virtualdisk.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/usb_descriptors.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/n64cartinterface.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/joybus.c
  )

target_include_directories(${PROJECT} PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  )


# Configure compilation flags and libraries for the example... see the corresponding function
# in hw/bsp/FAMILY/family.cmake for details.
family_configure_device_example(${PROJECT} noos)

target_link_libraries(${PROJECT} PUBLIC hardware_pio pico_stdlib pico_platform)

pico_add_extra_outputs(${PROJECT})
pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/src/joybus.pio)