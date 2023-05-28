cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

project(rescue)

if (${CMAKE_PROJECT_NAME} STREQUAL ${PROJECT_NAME})
    option(rescue_INSTALL "Generate the install target" ON)
else ()
    option(rescue_INSTALL "Generate the install target" OFF)
endif ()

get_filename_component(PROJECT_ROOT . ABSOLUTE)

add_executable(bootstrap EXCLUDE_FROM_ALL src/rescue.c src/deflate.c)
target_compile_definitions(bootstrap PUBLIC "-DRESCUE_BOOTSTRAP=\"${PROJECT_ROOT}/src/\"")

add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/resources.c
                   COMMAND bootstrap ARGS -o ${CMAKE_CURRENT_BINARY_DIR}/resources.c ${PROJECT_ROOT}/src/inflate.c ${PROJECT_ROOT}/src/template.c
                   DEPENDS bootstrap ${PROJECT_ROOT}/src/inflate.c ${PROJECT_ROOT}/src/template.c
                   WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                   COMMENT "Generating ${CMAKE_CURRENT_BINARY_DIR}/resources.c file")

add_executable(rescue src/rescue.c src/deflate.c ${CMAKE_CURRENT_BINARY_DIR}/resources.c)
target_include_directories(rescue PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

if (rescue_INSTALL)
    install(TARGETS rescue RUNTIME DESTINATION bin)
endif ()