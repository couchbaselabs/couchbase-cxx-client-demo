cmake_minimum_required(VERSION 3.19 FATAL_ERROR)

project(couchbase_cxx_client_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CPM.cmake)

cpmaddpackage(
  NAME
  couchbase_cxx_client
  GIT_TAG
  0a172faa29cecb22a8774a49dd22748901d4f2d2
  VERSION
  1.0.0
  GITHUB_REPOSITORY
  "couchbaselabs/couchbase-cxx-client"
  OPTIONS
  "COUCHBASE_CXX_CLIENT_STATIC_BORINGSSL ON")

add_subdirectory(examples)
