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
  set(LATEST_COUCHBASE_CXX_CLIENT 1.3.1)

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

option(BUILD_OPENTELEMETRY_EXAMPLES "Build opentelemetry examples" TRUE)
if(BUILD_OPENTELEMETRY_EXAMPLES)
  if(NOT CPM_DOWNLOAD_VERSION)
    include(cmake/CPM.cmake)
  endif()

  # https://github.com/open-telemetry/opentelemetry-cpp/releases
  cpmaddpackage(
    NAME
    opentelemetry
    VERSION
    1.23.0
    GITHUB_REPOSITORY
    "open-telemetry/opentelemetry-cpp"
    EXCLUDE_FROM_ALL
    ON
    OPTIONS
    "protobuf_MSVC_STATIC_RUNTIME OFF"
    "OPENTELEMETRY_INSTALL OFF"
    "WITH_ABI_VERSION_1 OFF"
    "WITH_ABI_VERSION_2 ON"
    "WITH_BENCHMARK OFF"
    "WITH_EXAMPLES OFF"
    "WITH_FUNC_TESTS OFF"
    "WITH_OTLP_GRPC OFF"
    "WITH_OTLP_HTTP ON"
    "WITH_HTTP_CLIENT_CURL OFF"
    "WITH_PROMETHEUS OFF"
    "WITH_OPENTRACING OFF"
    "WITH_STL CXX17"
    "BUILD_TESTING OFF"
    "BUILD_SHARED_LIBS OFF"
    "CMAKE_C_VISIBILITY_PRESET hidden"
    "CMAKE_CXX_VISIBILITY_PRESET hidden"
    "CMAKE_POSITION_INDEPENDENT_CODE ON")
endif()

add_subdirectory(examples)
