#include <couchbase/cluster.hxx>
#include <couchbase/codec/tao_json_serializer.hxx>
#include <couchbase/logger.hxx>
#include <couchbase/query_string_query.hxx>

#include <tao/json.hpp>

#include <iostream>

struct program_config {
  std::string connection_string{ "couchbase://127.0.0.1" };
  std::string user_name{ "Administrator" };
  std::string password{ "password" };
  std::string bucket_name{ "travel-sample" };
  std::string scope_name{ "inventory" };
  std::optional<std::string> profile{};
  bool verbose{ false };

  static auto from_env() -> program_config;
  static auto quote(std::string val) -> std::string;
  void dump();
};

/**
 * This example assumes the following index exists on the `travel-sample`
 * bucket.
 *
 * curl -XPUT -H "Content-Type: application/json" -u <username>:<password> \
 *   http://<search_host>:8094/api/bucket/travel-sample/scope/inventory/index/travel-inventory-landmarks
 *   -d \
 *   '{
 *     "type": "fulltext-index",
 *     "name": "travel-sample.inventory.travel-inventory-landmarks",
 *     "sourceType": "gocbcore",
 *     "sourceName": "travel-sample",
 *     "planParams": {
 *       "maxPartitionsPerPIndex": 1024,
 *       "indexPartitions": 1
 *     },
 *     "params": {
 *       "doc_config": {
 *         "docid_prefix_delim": "",
 *         "docid_regexp": "",
 *         "mode": "scope.collection.type_field",
 *         "type_field": "type"
 *       },
 *       "mapping": {
 *         "analysis": {},
 *         "default_analyzer": "standard",
 *         "default_datetime_parser": "dateTimeOptional",
 *         "default_field": "_all",
 *         "default_mapping": {
 *           "dynamic": false,
 *           "enabled": false
 *         },
 *         "default_type": "_default",
 *         "docvalues_dynamic": true,
 *         "index_dynamic": true,
 *         "store_dynamic": true,
 *         "type_field": "_type",
 *         "types": {
 *           "inventory.landmark": {
 *             "default_analyzer": "standard",
 *             "dynamic": true,
 *             "enabled": true
 *           }
 *         }
 *       },
 *       "store": {
 *         "indexType": "scorch",
 *         "segmentVersion": 15
 *       }
 *     },
 *     "sourceParams": {}
 *   }'
 */

int
main()
{
  auto config = program_config::from_env();
  config.dump();

  if (config.verbose) {
    couchbase::logger::initialize_console_logger();
    couchbase::logger::set_level(couchbase::logger::log_level::trace);
  }

  auto options = couchbase::cluster_options(config.user_name, config.password);
  if (config.profile) {
    options.apply_profile(config.profile.value());
  }

  auto [connect_err, cluster] =
    couchbase::cluster::connect(config.connection_string, options).get();
  if (connect_err) {
    std::cout << "Unable to connect to the cluster. ec: " << connect_err.message() << "\n";
    return EXIT_FAILURE;
  }
  auto scope = cluster.bucket(config.bucket_name).scope(config.scope_name);

  auto [err, resp] = scope
                       .search("travel-inventory-landmarks",
                               couchbase::search_request(couchbase::query_string_query("nice bar")),
                               couchbase::search_options{}.fields({ "content" }))
                       .get();

  for (const auto& row : resp.rows()) {
    auto fields = row.fields_as<couchbase::codec::tao_json_serializer>();
    std::cout << "score: " << row.score() << ", id: \"" << row.id() << "\", content: \""
              << fields["content"].get_string() << "\"\n";
  }

  cluster.close().get();

  return 0;
}

auto
program_config::from_env() -> program_config
{
  program_config config{};

  if (const auto* val = getenv("CONNECTION_STRING"); val != nullptr) {
    config.connection_string = val;
  }
  if (const auto* val = getenv("USER_NAME"); val != nullptr) {
    config.user_name = val;
  }
  if (const auto* val = getenv("PASSWORD"); val != nullptr) {
    config.password = val;
  }
  if (const auto* val = getenv("BUCKET_NAME"); val != nullptr) {
    config.bucket_name = val;
  }
  if (const auto* val = getenv("SCOPE_NAME"); val != nullptr) {
    config.scope_name = val;
  }
  if (const auto* val = getenv("PROFILE"); val != nullptr) {
    config.profile = val; // e.g. "wan_development"
  }
  if (const auto* val = getenv("VERBOSE"); val != nullptr) {
    const std::array<std::string, 5> truthy_values = {
      "yes", "y", "on", "true", "1",
    };
    for (const auto& truth : truthy_values) {
      if (val == truth) {
        config.verbose = true;
        break;
      }
    }
  }

  return config;
}

auto
program_config::quote(std::string val) -> std::string
{
  return "\"" + val + "\"";
}

void
program_config::dump()
{
  std::cout << "  CONNECTION_STRING: " << quote(connection_string) << "\n";
  std::cout << "          USER_NAME: " << quote(user_name) << "\n";
  std::cout << "           PASSWORD: [HIDDEN]\n";
  std::cout << "        BUCKET_NAME: " << quote(bucket_name) << "\n";
  std::cout << "         SCOPE_NAME: " << quote(scope_name) << "\n";
  std::cout << "            VERBOSE: " << std::boolalpha << verbose << "\n";
  std::cout << "            PROFILE: " << (profile ? quote(*profile) : "[NONE]") << "\n\n";
}
