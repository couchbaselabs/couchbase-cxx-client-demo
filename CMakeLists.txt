cmake_minimum_required(VERSION 3.19 FATAL_ERROR)

project(couchbase_cxx_client_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CPM.cmake)

cpmaddpackage(
  NAME
  couchbase_cxx_client
  GIT_TAG
  76c30a927e81f8cc58f00c1cdba2f996c8c24385
  VERSION
  1.0.0
  GITHUB_REPOSITORY
  "couchbaselabs/couchbase-cxx-client"
  OPTIONS
  "COUCHBASE_CXX_CLIENT_STATIC_BORINGSSL ON")

add_executable(minimal minimal.cpp)
target_link_libraries(minimal couchbase_cxx_client::couchbase_cxx_client
                      taocpp::json)
