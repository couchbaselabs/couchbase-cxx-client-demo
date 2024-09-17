cmake_minimum_required(VERSION 3.19 FATAL_ERROR)

project(couchbase_cxx_client_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(fmt REQUIRED)
find_package(couchbase_cxx_client REQUIRED)

if (couchbase_cxx_client_FOUND)
  find_package(taocpp-json REQUIRED)
else()
  include(cmake/CPM.cmake)

  cpmaddpackage(
    NAME
    couchbase_cxx_client
    GIT_TAG
    1.0.1
    VERSION
    1.0.1
    GITHUB_REPOSITORY
    "couchbase/couchbase-cxx-client"
    OPTIONS
    "COUCHBASE_CXX_CLIENT_STATIC_BORINGSSL ON")
endif()

add_subdirectory(examples)
