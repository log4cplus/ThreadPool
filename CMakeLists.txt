cmake_minimum_required(VERSION 3.14)

include(cmake/prelude.cmake)

project(
    ThreadPool
    VERSION 0.1.0
    DESCRIPTION "A simple C++11 Thread Pool implementation"
    HOMEPAGE_URL "https://github.com/log4cplus/ThreadPool"
    LANGUAGES NONE
)

docs_early_return()

include(cmake/project-is-top-level.cmake)
include(cmake/variables.cmake)

# ---- Declare library ----

add_library(ThreadPool_ThreadPool INTERFACE)
add_library(ThreadPool::ThreadPool ALIAS ThreadPool_ThreadPool)

set_property(
    TARGET ThreadPool_ThreadPool PROPERTY
    EXPORT_NAME ThreadPool
)

target_include_directories(
    ThreadPool_ThreadPool ${warning_guard}
    INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)

#target_compile_features(ThreadPool_ThreadPool INTERFACE cxx_std_17)

# ---- Install rules ----

if(NOT CMAKE_SKIP_INSTALL_RULES)
  include(cmake/install-rules.cmake)
endif()

# ---- Examples ----

if(PROJECT_IS_TOP_LEVEL)
  option(BUILD_EXAMPLES "Build examples tree." "${ThreadPool_DEVELOPER_MODE}")
  if(BUILD_EXAMPLES)
    add_subdirectory(example)
  endif()
endif()

# ---- Developer mode ----

if(NOT ThreadPool_DEVELOPER_MODE)
  return()
elseif(NOT PROJECT_IS_TOP_LEVEL)
  message(
      AUTHOR_WARNING
      "Developer mode is intended for developers of ThreadPool"
  )
endif()

include(cmake/dev-mode.cmake)
