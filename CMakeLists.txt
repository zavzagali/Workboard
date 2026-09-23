cmake_minimum_required(VERSION 3.21)
project(WorkBoard LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Slint QUIET)

if (NOT Slint_FOUND)
    message(STATUS "Slint not found, downloading from Git...")
    include(FetchContent)
    FetchContent_Declare(
        Slint
        GIT_REPOSITORY https://github.com/slint-ui/slint.git
        GIT_TAG release/1
        SOURCE_SUBDIR api/cpp
    )
    FetchContent_MakeAvailable(Slint)
endif()

add_executable(WorkBoard src/main.cpp src/storage.h)
target_link_libraries(WorkBoard PRIVATE Slint::Slint)
slint_target_sources(WorkBoard ui/app.slint)

if (WIN32)
    add_custom_command(TARGET WorkBoard POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_RUNTIME_DLLS:WorkBoard> $<TARGET_FILE_DIR:WorkBoard>
        COMMAND_EXPAND_LISTS)
endif()
