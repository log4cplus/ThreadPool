cmake_minimum_required(VERSION 3.20)
project(ThreadPoolTests LANGUAGES CXX)

enable_testing()
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 11)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# CMake versions shipped on hosted runners may map MSVC C++23 to c++latest.
# Select the bounded C++23 preview mode instead of enabling later standards.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_CXX_STANDARD EQUAL 23)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("/std:c++23preview" THREADPOOL_HAS_CXX23_PREVIEW)
    if(NOT THREADPOOL_HAS_CXX23_PREVIEW)
        message(FATAL_ERROR "This MSVC toolset does not support /std:c++23preview")
    endif()
    set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "/std:c++23preview")
    set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "/std:c++23preview")
endif()

function(threadpool_test_suite name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(${name} PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    foreach(test_case IN LISTS ARGN)
        add_test(NAME ${name}.${test_case} COMMAND ${name} ${test_case})
        set_tests_properties(${name}.${test_case} PROPERTIES TIMEOUT 90)
    endforeach()
endfunction()

threadpool_test_suite(portable
    results zero resize busy-resize worker-resize concurrent drain
    queue-full enqueue-block enqueue-block-growth wait-empty wait-idle
    shutdown-active shutdown-retired)

# These suites instrument the real Linux pthread entry points. They must not
# be replaced by a mock thread implementation or compiled on other platforms.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    threadpool_test_suite(worker_failure
        construct construct-reserve zero first partial downsizing reserve
        allocation tasks)
    threadpool_test_suite(worker_shutdown
        shutdown-active shutdown-retired shutdown-native-active
        shutdown-native-retired independent worker-resize resize-worker
        resize-concurrent resize-native construct lazy shrink-failure
        shrink-worker-failure failed-first failed-growth failed-several
        failed-existing autonomous shutdown-joiner shutdown-promoted
        shutdown-immediate concurrent)
    target_link_libraries(worker_failure PRIVATE ${CMAKE_DL_LIBS})
    target_link_libraries(worker_shutdown PRIVATE ${CMAKE_DL_LIBS})
endif()
