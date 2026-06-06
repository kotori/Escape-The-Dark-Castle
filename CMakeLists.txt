cmake_minimum_required(VERSION 3.16)
project(EscapeDarkCastle C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Gather default game source files
set(GAME_SOURCES
    src/edc.cpp
    src/engine.cpp
)

# 2. Configure Windows Specific Core Static Dependencies
if(WIN32 AND MINGW)
    # Include the compiled Windows resource asset layout
    list(APPEND GAME_SOURCES resources.rc)
    
    # Compile the raw SQLite source file directly into your executable target!
    set(SQLITE_SOURCE_FILE /usr/x86_64-w64-mingw32/include/sqlite3.c)
    list(APPEND GAME_SOURCES ${SQLITE_SOURCE_FILE})
    
    # FIX: Force the compiler to process this specific file as pure C code!
    set_source_files_properties(${SQLITE_SOURCE_FILE} PROPERTIES LANGUAGE C)
endif()

# 3. Define our binary executable target name with the full source stack
add_executable(escape_dark_castle ${GAME_SOURCES})

# Enforce fully static runtime linkage configurations
if(WIN32 AND MINGW)
    set_target_properties(escape_dark_castle PROPERTIES LINK_FLAGS "-static -static-libgcc -static-libstdc++")
endif()


# ==============================================================================
# 1. DATABASE DEPENDENCY (SQLite3)
# ==============================================================================
if(WIN32 AND MINGW)
    # The sqlite3.c file is bundled directly above, just inform CXX of its headers
    target_include_directories(escape_dark_castle PRIVATE /usr/x86_64-w64-mingw32/include)
else()
    # NATIVE LINUX BUILD FALLBACK
    find_package(SQLite3 REQUIRED)
    target_link_libraries(escape_dark_castle PRIVATE SQLite::SQLite3)
endif()


# ==============================================================================
# 2. MULTIMEDIA DEPENDENCY (Allegro 5)
# ==============================================================================
if(WIN32 AND MINGW)
    message(STATUS "Cross-Compiling Windows Target: Searching system paths for Allegro 5...")

    find_path(ALLEGRO_INCLUDE_DIR NAMES allegro5/allegro.h)
    find_library(ALLEGRO_MONOLITH_LIB NAMES allegro_monolith-static)

    if(ALLEGRO_INCLUDE_DIR AND ALLEGRO_MONOLITH_LIB)
        target_compile_definitions(escape_dark_castle PRIVATE ALLEGRO_STATICLINK)
        target_include_directories(escape_dark_castle PRIVATE ${ALLEGRO_INCLUDE_DIR})

        target_link_libraries(escape_dark_castle PRIVATE
            ${ALLEGRO_MONOLITH_LIB}
            freetype png16 jpeg webp vorbisfile vorbis ogg FLAC sharpyuv zlib openmpt opusfile opus dumb physfs
            opengl32 dsound gdi32 winmm ole32 psapi shlwapi -pthread
        )
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALLEGRO REQUIRED allegro-5 allegro_image-5 allegro_primitives-5 allegro_font-5 allegro_ttf-5 allegro_audio-5 allegro_acodec-5)
    target_compile_options(escape_dark_castle PRIVATE ${ALLEGRO_CFLAGS_OTHER})
    target_include_directories(escape_dark_castle PRIVATE ${ALLEGRO_INCLUDE_DIRS})
    target_link_libraries(escape_dark_castle PRIVATE ${ALLEGRO_LIBRARIES})
endif()

# ==============================================================================
# 3. AUTOMATED CROSS-PLATFORM ASSET PLACEMENT
# ==============================================================================
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data.seed")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/data.seed" "${CMAKE_CURRENT_BINARY_DIR}/data.seed" COPYONLY)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/assets" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/data" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
endif()
if(WIN32)
    set_target_properties(escape_dark_castle PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
