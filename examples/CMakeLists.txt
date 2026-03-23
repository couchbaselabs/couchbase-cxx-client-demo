add_executable(minimal minimal.cpp)
target_link_libraries(minimal PRIVATE ${COUCHBASE_LIBRARY} taocpp::json)

add_executable(minimal_query minimal_query.cpp)
target_link_libraries(minimal_query PRIVATE ${COUCHBASE_LIBRARY} taocpp::json)

add_executable(minimal_search minimal_search.cpp)
target_link_libraries(minimal_search PRIVATE ${COUCHBASE_LIBRARY} taocpp::json)

add_executable(transactions_transfer_basic transactions_transfer_basic.cpp)
target_link_libraries(transactions_transfer_basic PRIVATE ${COUCHBASE_LIBRARY}
                                                          taocpp::json)

add_executable(transactions_transfer_read_replica
               transactions_transfer_read_replica.cpp)
target_link_libraries(transactions_transfer_read_replica
                      PRIVATE ${COUCHBASE_LIBRARY} taocpp::json)

add_executable(transactions_transfer_with_binary_objects
               transactions_transfer_with_binary_objects.cpp)
target_link_libraries(transactions_transfer_with_binary_objects
                      PRIVATE ${COUCHBASE_LIBRARY} taocpp::json)

add_executable(ledger_with_csv_encoding ledger_with_csv_encoding.cpp)
target_link_libraries(ledger_with_csv_encoding PRIVATE ${COUCHBASE_LIBRARY}
                                                       taocpp::json fmt::fmt)

add_executable(minimal_with_char_array minimal_with_char_array.cpp)
target_link_libraries(minimal_with_char_array PRIVATE ${COUCHBASE_LIBRARY})

if(BUILD_OPENTELEMETRY_EXAMPLES)
  add_executable(inventory_with_opentelemetry inventory_with_opentelemetry.cpp)
  target_link_libraries(
    inventory_with_opentelemetry
    PRIVATE ${COUCHBASE_LIBRARY} taocpp::json opentelemetry_metrics
            opentelemetry_trace opentelemetry_exporter_otlp_http
            opentelemetry_exporter_otlp_http_metric)
endif()
