cmake_minimum_required(VERSION 3.19 FATAL_ERROR)

project(couchbase_cxx_client_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(fmt REQUIRED)
find_package(couchbase_cxx_client)

if (couchbase_cxx_client_FOUND)
  find_package(taocpp-json REQUIRED)
else()
  set(LATEST_COUCHBASE_CXX_CLIENT 1.0.4)

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
    "COUCHBASE_CXX_CLIENT_INSTALL ON"
    "COUCHBASE_CXX_CLIENT_BUILD_SHARED ON"
    "COUCHBASE_CXX_CLIENT_BUILD_STATIC OFF")
endif()

add_subdirectory(examples)
