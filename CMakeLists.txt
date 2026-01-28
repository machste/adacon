project(adacon)

SET(PROJECT_VERSION 0.1.0)
SET(PROJECT_TITLE "Adaura Controller")

cmake_minimum_required(VERSION 3.12)

find_package(PkgConfig REQUIRED)

pkg_check_modules(MODULES REQUIRED libmasc>=1.5.1 ncurses)

include(GNUInstallDirs)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(UNIX)
    set(CMAKE_INSTALL_PREFIX /usr)
endif(UNIX)

option(DEBUG "Enable Debugging (GDB support and no optimisation)." OFF)
option(LOG_ENABLED "Use logging for more detailed information." ON)

if(DEBUG)
	add_definitions(-O0 -ggdb3)
else()
	add_definitions(-Os)
endif()
add_definitions(-Wall -Werror -std=gnu11)

configure_file(config.h.in ${PROJECT_BINARY_DIR}/config.h)
add_definitions("-include config.h")

file(GLOB SRC CONFIGURE_DEPENDS "*.h" "*.c")

add_executable(adacon ${SRC})
target_link_libraries(adacon ${MODULES_LIBRARIES})
target_include_directories(adacon PRIVATE ${MODULES_INCLUDE_DIRS})
target_compile_options(adacon PRIVATE ${MODULES_CFLAGS_OTHER})

install(TARGETS adacon RUNTIME DESTINATION /usr/bin)
