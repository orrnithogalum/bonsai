cmake_minimum_required(VERSION 3.14)
project(bonsai)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui.git
  GIT_TAG v7.0.3
)
FetchContent_MakeAvailable(ftxui)

set(SRC
    src/main.cpp

    src/core/app_data.cpp
    src/core/scanner.cpp

    src/ui/menu.cpp
    src/ui/modal_confirm.cpp
    src/ui/modal_error.cpp
    src/ui/piechart.cpp
)

# Embed default config
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gen)

set(GENERATED_HEADER_NAME defaults)
set(DEFAULT_CONF ${CMAKE_CURRENT_SOURCE_DIR}/res/bonsai.default.conf)
set(GENERATED_HEADER ${CMAKE_CURRENT_BINARY_DIR}/gen/${GENERATED_HEADER_NAME}.hpp)

add_custom_command(
    OUTPUT ${GENERATED_HEADER}

    COMMAND xxd -i -n bonsai_default ${DEFAULT_CONF} > ${GENERATED_HEADER}
    COMMAND sed -i -E "s/^(unsigned )/inline \\1/" ${GENERATED_HEADER}

    DEPENDS ${DEFAULT_CONF}
    COMMENT "Embedding default config into ${GENERATED_HEADER}"
    VERBATIM
)

add_custom_target(generate_bonsai_conf_header ALL DEPENDS ${GENERATED_HEADER})
include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_executable(bonsai ${SRC})
add_dependencies(bonsai generate_bonsai_conf_header)

target_compile_options(bonsai PRIVATE
    $<$<CONFIG:Release>:-O3>
)

target_link_libraries(bonsai
    ftxui::screen
    ftxui::dom
    ftxui::component
)
