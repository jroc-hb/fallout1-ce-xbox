# Build static lib only
set(BUILD_SHARED_LIBS OFF)
set(SDL_SHARED OFF)
set(SDL_STATIC ON)

# Fixes _ftol2_sse already defined
set(SDL_LIBC ON)

if (ANDROID)
    set(SDL_STATIC_PIC ON)
endif()

include(FetchContent)

FetchContent_Declare(sdl3
    GIT_REPOSITORY "https://github.com/libsdl-org/SDL"
    GIT_TAG "release-3.4.10"
)

FetchContent_GetProperties(sdl3)
if (NOT sdl3_POPULATED)
    FetchContent_Populate(sdl3)
endif()

add_subdirectory(${sdl3_SOURCE_DIR} ${sdl3_BINARY_DIR} EXCLUDE_FROM_ALL)
