add_executable(minimal minimal.cpp)
target_link_libraries(minimal couchbase_cxx_client::couchbase_cxx_client
                      taocpp::json)
