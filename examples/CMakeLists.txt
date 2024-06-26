add_executable(minimal minimal.cpp)
target_link_libraries(minimal
    PRIVATE couchbase_cxx_client::couchbase_cxx_client taocpp::json)

add_executable(transactions_transfer_basic transactions_transfer_basic.cpp)
target_link_libraries(transactions_transfer_basic
    PRIVATE couchbase_cxx_client::couchbase_cxx_client taocpp::json)

add_executable(transactions_transfer_read_replica transactions_transfer_read_replica.cpp)
target_link_libraries(transactions_transfer_read_replica
    PRIVATE couchbase_cxx_client::couchbase_cxx_client taocpp::json)
