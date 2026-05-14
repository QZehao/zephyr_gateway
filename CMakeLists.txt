# zephyr_gateway — 仓库根 Zephyr 应用入口（支持 west build -b … .）
# 通过 add_subdirectory(framework) 使用 zephyr_framework；业务由树外模块 modules/zephyr_gateway 提供。
#
# Windows：find_package(Zephyr) 与 project() 须在本文件顶层执行，不能仅在子目录执行。
cmake_minimum_required(VERSION 3.20.0)

if(NOT CONF_FILE)
  set(CONF_FILE
      "${CMAKE_CURRENT_LIST_DIR}/framework/prj.conf;${CMAKE_CURRENT_LIST_DIR}/gateway_prj.conf"
      CACHE STRING "Zephyr application config files" FORCE)
endif()

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/framework/app.overlay")
  list(APPEND DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/framework/app.overlay")
endif()

if(NOT DEFINED ENV{ZEPHYR_EXTRA_MODULES} OR "$ENV{ZEPHYR_EXTRA_MODULES}" STREQUAL "")
  set(ENV{ZEPHYR_EXTRA_MODULES} "${CMAKE_CURRENT_LIST_DIR}/modules/zephyr_gateway")
endif()

if(NOT DEFINED ENV{ZEPHYR_BASE})
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/framework/zephyr_config.env")
    message(STATUS "Loading Zephyr configuration from framework/zephyr_config.env...")
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/framework/zephyr_config.env" CONFIG_LINES)
    foreach(LINE ${CONFIG_LINES})
      if(NOT LINE MATCHES "^#" AND NOT LINE MATCHES "^$")
        string(REGEX MATCH "^([^=]+)=(.*)$" MATCH ${LINE})
        if(MATCH)
          set(ENV{${CMAKE_MATCH_1}} "${CMAKE_MATCH_2}")
        endif()
      endif()
    endforeach()
  else()
    message(
      FATAL_ERROR
        "ZEPHYR_BASE not set and framework/zephyr_config.env not found.\n"
        "Copy framework/zephyr_config.env.template to framework/zephyr_config.env, "
        "or export ZEPHYR_BASE.")
  endif()
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# 版本：优先应用仓库根目录 APP_VERSION；否则使用 framework/APP_VERSION；再无则默认 1.0.0
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/APP_VERSION")
  file(READ "${CMAKE_CURRENT_LIST_DIR}/APP_VERSION" PROJECT_VERSION_RAW)
  string(STRIP "${PROJECT_VERSION_RAW}" PROJECT_VERSION_RAW)
  if(PROJECT_VERSION_RAW MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(PROJECT_VERSION_MAJOR ${CMAKE_MATCH_1})
    set(PROJECT_VERSION_MINOR ${CMAKE_MATCH_2})
    set(PROJECT_VERSION_PATCH ${CMAKE_MATCH_3})
    set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")
    message(STATUS "Project version from application APP_VERSION: ${PROJECT_VERSION}")
  else()
    message(FATAL_ERROR "APP_VERSION must contain a single line X.Y.Z, got: '${PROJECT_VERSION_RAW}'")
  endif()
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/framework/APP_VERSION")
  file(READ "${CMAKE_CURRENT_LIST_DIR}/framework/APP_VERSION" PROJECT_VERSION_RAW)
  string(STRIP "${PROJECT_VERSION_RAW}" PROJECT_VERSION_RAW)
  if(PROJECT_VERSION_RAW MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(PROJECT_VERSION_MAJOR ${CMAKE_MATCH_1})
    set(PROJECT_VERSION_MINOR ${CMAKE_MATCH_2})
    set(PROJECT_VERSION_PATCH ${CMAKE_MATCH_3})
    set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")
    message(STATUS "Project version from framework/APP_VERSION: ${PROJECT_VERSION}")
  else()
    message(
      FATAL_ERROR
        "framework/APP_VERSION must contain a single line X.Y.Z, got: '${PROJECT_VERSION_RAW}'")
  endif()
else()
  set(PROJECT_VERSION_MAJOR 1)
  set(PROJECT_VERSION_MINOR 0)
  set(PROJECT_VERSION_PATCH 0)
  set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")
  message(STATUS "Project version default (no APP_VERSION): ${PROJECT_VERSION}")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zephyr_gateway VERSION ${PROJECT_VERSION})

set(ZEPHYR_GATEWAY_TOPLEVEL_BOOTSTRAP ON)
add_subdirectory(framework)
