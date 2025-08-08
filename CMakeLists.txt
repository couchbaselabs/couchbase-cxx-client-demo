cmake_minimum_required(VERSION 3.19 FATAL_ERROR)

project(couchbase_cxx_client_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(fmt REQUIRED)

option(USE_STATIC "Use static library for Couchbase SDK" FALSE)
if(USE_STATIC)
  find_package(couchbase_cxx_client_static)
  set(COUCHBASE_LIBRARY couchbase_cxx_client::couchbase_cxx_client_static)
else()
  find_package(couchbase_cxx_client)
  set(COUCHBASE_LIBRARY couchbase_cxx_client::couchbase_cxx_client)
endif()

if(couchbase_cxx_client_FOUND OR couchbase_cxx_client_static_FOUND)
  find_package(taocpp-json REQUIRED)
else()
  set(LATEST_COUCHBASE_CXX_CLIENT 1.2.0)

  message(
    STATUS
      "Unable to find ${COUCHBASE_LIBRARY} installed system-wide. Use CPM.cmake to fetch ${LATEST_COUCHBASE_CXX_CLIENT} from git repository"
  )

  include(cmake/CPM.cmake)

  cpmaddpackage(
    NAME
    couchbase_cxx_client
    GIT_TAG
    ${LATEST_COUCHBASE_CXX_CLIENT}
    VERSION
    ${LATEST_COUCHBASE_CXX_CLIENT}
    GITHUB_REPOSITORY
    "couchbase/couchbase-cxx-client"
    OPTIONS
    "COUCHBASE_CXX_CLIENT_BUILD_SHARED OFF"
    "COUCHBASE_CXX_CLIENT_BUILD_STATIC ON")

  set(COUCHBASE_LIBRARY couchbase_cxx_client::couchbase_cxx_client_static)
endif()

if(TARGET ${COUCHBASE_LIBRARY})
  message(STATUS "Using target ${COUCHBASE_LIBRARY} for Couchbase C++ SDK")
else()
  message(
    FATAL_ERROR "Unable to find ${COUCHBASE_LIBRARY} for Couchbase C++ SDK")
endif()

add_subdirectory(examples)
