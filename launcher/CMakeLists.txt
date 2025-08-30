set(XBE_TITLE "fallout-launcher")
set(XBOX_ISO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../fallout-ce)

cmake_minimum_required(VERSION 3.13)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

project(fallout-launcher)

set(EXECUTABLE_NAME fallout-launcher)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Compiler flags
set(CMAKE_C_FLAGS_DEBUG "-g -Wall -Wextra")
set(CMAKE_C_FLAGS_RELEASE "-Ofast")

# Avoid macro spam
add_compile_options(-Wno-builtin-macro-redefined)
add_definitions(-DXBOX -DNXDK)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED YES)
set(CMAKE_C_EXTENSIONS NO)

# Setup SDL2 (for NXDK, use pkg-config)
if(NXDK)
    include(FindPkgConfig)
    pkg_search_module(SDL2 REQUIRED sdl2)
    pkg_search_module(SDL2_TTF REQUIRED SDL2_ttf)
    # Optional: SDL_image if needed for PNG support
    pkg_search_module(SDL2_IMAGE REQUIRED SDL2_image)
endif()

# Source files
add_executable(${EXECUTABLE_NAME} WIN32
    src/main.c
)

target_include_directories(${EXECUTABLE_NAME} PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
    ${SDL2_IMAGE_INCLUDE_DIRS}
)

target_link_libraries(${EXECUTABLE_NAME}
    ${SDL2_LIBRARIES}
    ${SDL2_TTF_LIBRARIES}
    ${SDL2_IMAGE_LIBRARIES}
)

# Xbox post-build conversion
if (NXDK)
    add_custom_target(cxbe_convert_launcher ALL
        VERBATIM COMMAND "${CMAKE_COMMAND}" -E env ./tools/cxbe/cxbe -OUT:${XBOX_ISO_DIR}/default.xbe -TITLE:${XBE_TITLE} ${CMAKE_CURRENT_BINARY_DIR}/${XBE_TITLE}.exe
        WORKING_DIRECTORY ${NXDK_DIR}
        COMMENT "CXBE Conversion (Launcher): [EXE -> XBE]"
    )
    add_dependencies(cxbe_convert_launcher fallout-launcher)
endif()
