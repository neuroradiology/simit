# Simit
set(CMAKE_MACOSX_RPATH 1)

if (SIMIT_SHARED_LIBRARY)
  set(SIMIT_LIBRARY_TYPE SHARED)
  message("-- Shared library")
else()
  set(SIMIT_LIBRARY_TYPE STATIC)
  message("-- Static library")
endif()

set(SIMIT_SOURCE_DIRS . util)

foreach(dir ${SIMIT_SOURCE_DIRS})
  file(GLOB SIMIT_HEADERS ${SIMIT_HEADERS} ${dir}/*.h)
  file(GLOB SIMIT_SOURCES ${SIMIT_SOURCES} ${dir}/*.cpp)
endforeach()

set(SIMIT_HEADERS ${SIMIT_HEADERS})
set(SIMIT_SOURCES ${SIMIT_SOURCES})

add_subdirectory(backend)
add_subdirectory(frontend)
add_subdirectory(lower)
add_subdirectory(visualizer)

add_definitions(${SIMIT_DEFINITIONS})
include_directories(${SIMIT_INCLUDE_DIRS})
add_library(${PROJECT_NAME} ${SIMIT_LIBRARY_TYPE} ${SIMIT_HEADERS} ${SIMIT_SOURCES})
target_link_libraries(${PROJECT_NAME} ${SIMIT_LIBRARIES})


# LLVM
if (DEFINED ENV{LLVM_CONFIG})
  set(LLVM_CONFIG $ENV{LLVM_CONFIG})
else()
  set(LLVM_CONFIG "llvm-config")
endif()

execute_process(COMMAND ${LLVM_CONFIG} --version RESULT_VARIABLE LLVM_CONFIG_RESULT OUTPUT_VARIABLE LLVM_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT ${LLVM_CONFIG_RESULT} STREQUAL "0")
  if (DEFINED ENV{LLVM_CONFIG})
    message(FATAL_ERROR "Could not execute llvm-config (LLVM_CONFIG=${LLVM_CONFIG}). Add llvm-config version 3.4.2 to PATH or set the LLVM_CONFIG environment variable to point to it.")
  else()
    message(FATAL_ERROR "Could not execute llvm-config. Add llvm-config version 3.3 or higher to PATH or set the LLVM_CONFIG environment variable to point to it.")
  endif()
endif()

if (NOT ${LLVM_VERSION} MATCHES "^([3-9]|[0-9][0-9]+)\\.([3-9]|[0-9][0-9]+)(\\.[0-9]+)?(svn)?$")
  message(FATAL_ERROR "Need LLVM version 3.3 or higher. Found ${LLVM_CONFIG} version \"${LLVM_VERSION}\".")
elseif (${LLVM_VERSION} MATCHES "^([3-9]|[0-9][0-9]+)\\.([5-9]|[0-9][0-9]+)(\\.[0-9]+)?(svn)?$")
  set(LLVM_CONFIG_GT_45 1)
endif()

message("-- Found LLVM: ${LLVM_CONFIG} (version \"${LLVM_VERSION}\")")

string(SUBSTRING ${LLVM_VERSION} 0 1 LLVM_MAJOR_VERSION)
string(SUBSTRING ${LLVM_VERSION} 2 1 LLVM_MINOR_VERSION)
add_definitions("-DLLVM_MAJOR_VERSION=${LLVM_MAJOR_VERSION}")
add_definitions("-DLLVM_MINOR_VERSION=${LLVM_MINOR_VERSION}")

string(SUBSTRING ${LLVM_VERSION} 0 3 LLVM_VERSION)
string(REPLACE "." "" LLVM_VERSION "${LLVM_VERSION}")

execute_process(COMMAND ${LLVM_CONFIG} --cppflags OUTPUT_VARIABLE LLVM_CPPFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
set_property(TARGET ${PROJECT_NAME} PROPERTY COMPILE_FLAGS ${LLVM_CPPFLAGS})

execute_process(COMMAND ${LLVM_CONFIG} --includedir OUTPUT_VARIABLE LLVM_INCLUDES OUTPUT_STRIP_TRAILING_WHITESPACE)
include_directories("${LLVM_INCLUDES}")

set(LLVM_COMPONENTS core mcjit bitwriter x86 ipo)
if (LLVM_VERSION GREATER 36)
 list(APPEND LLVM_COMPONENTS passes)
else()
 list(APPEND LLVM_COMPONENTS ipo)
endif()

execute_process(COMMAND "${LLVM_CONFIG}" --libfiles ${LLVM_COMPONENTS} OUTPUT_VARIABLE LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
separate_arguments(LLVM_LIBS)  # Turn the space-separate string into a ;-separated list
target_link_libraries(${PROJECT_NAME} PRIVATE ${LLVM_LIBS})

if (${LLVM_VERSION} GREATER 34)
  execute_process(COMMAND "${LLVM_CONFIG}" --system-libs ${LLVM_COMPONENTS} OUTPUT_VARIABLE EXTRA_LIBS)
else()
  execute_process(COMMAND "${LLVM_CONFIG}" --ldflags OUTPUT_VARIABLE EXTRA_LIBS)
endif()
string(STRIP EXTRA_LIBS "${EXTRA_LIBS}")
string(REPLACE "-l" ";" EXTRA_LIBS "${EXTRA_LIBS}")
string(REPLACE "\n" "" EXTRA_LIBS "${EXTRA_LIBS}")
string(REPLACE " " "" EXTRA_LIBS "${EXTRA_LIBS}")
target_link_libraries(${PROJECT_NAME} PUBLIC ${EXTRA_LIBS})


# EIGEN
if (DEFINED ENV{EIGEN3_INCLUDE_DIR})
  include_directories($ENV{EIGEN3_INCLUDE_DIR})
else ()
  find_package(Eigen3)
  if (EIGEN3_FOUND)
    add_definitions(-DEIGEN)
    include_directories( ${EIGEN3_INCLUDE_DIR} )
  endif()
endif()

