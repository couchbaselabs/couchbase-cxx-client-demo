#include <couchbase/cluster.hxx> // Core SDK entry point: cluster, bucket, collection
#include <couchbase/codec/tao_json_serializer.hxx> // JSON serialization via tao/json
#include <couchbase/logger.hxx>                    // Optional SDK-level logging

#include <tao/json.hpp> // JSON value type used for document content

#include <iostream>

// Holds connection parameters; defaults work against a local Couchbase instance.
struct program_config {
  std::string connection_string{ "couchbase://127.0.0.1" };
  std::string user_name{ "Administrator" };
  std::string password{ "password" };
  std::string bucket_name{ "default" };
  std::string scope_name{ couchbase::scope::default_name };           // "_default"
  std::string collection_name{ couchbase::collection::default_name }; // "_default"
  std::optional<std::string> profile{}; // e.g. "wan_development" for high-latency tuning
  bool verbose{ false };

  static auto from_env() -> program_config;
  static auto quote(std::string val) -> std::string;
  void dump();
};

int
main()
{
  auto config = program_config::from_env(); // Load config from environment variables
  config.dump();

  if (config.verbose) {
    // Enable SDK trace logging to stdout — useful for diagnosing connectivity issues
    couchbase::logger::initialize_console_logger();
    couchbase::logger::set_level(couchbase::logger::log_level::trace);
  }

  // Cluster options carry credentials and optional performance profiles
  auto options = couchbase::cluster_options(config.user_name, config.password);
  if (config.profile) {
    options.apply_profile(config.profile.value()); // Tune timeouts for your network conditions
  }

  // Connect to the cluster; returns a (error, cluster) pair via structured bindings
  auto [connect_err, cluster] =
    couchbase::cluster::connect(config.connection_string, options).get();
  if (connect_err) {
    std::cout << "Unable to connect to the cluster. ec: " << connect_err.message() << "\n";
  } else {
    // Navigate the data hierarchy: cluster → bucket → scope → collection
    auto collection = cluster.bucket(config.bucket_name)
                        .scope(config.scope_name)
                        .collection(config.collection_name);

    const std::string document_id{ "minimal_example" };

    {
      // upsert: insert or replace the document; no prior existence required
      const tao::json::value basic_doc{
        { "a", 1.0 },
        { "b", 2.0 },
      };

      auto [err, resp] = collection.upsert(document_id, basic_doc, {}).get();
      std::cout << "Upsert id: " << document_id;
      if (err.ec()) {
        std::cout << ", Error: " << err.message() << "\n";
      } else {
        // CAS (Compare-And-Swap) is a version token used for optimistic concurrency control
        std::cout << ", CAS: " << resp.cas().value() << "\n";
      }
    }
    {
      // get: fetch a document by its ID
      auto [err, resp] = collection.get(document_id, {}).get();
      std::cout << "Get id: " << document_id;
      if (err.ec()) {
        std::cout << ", Error: " << err.message() << "\n";
      } else {
        std::cout << ", CAS: " << resp.cas().value() << "\n";
        // content_as<T> deserializes the raw bytes into the requested type
        std::cout << tao::json::to_string(resp.content_as<tao::json::value>()) << "\n";
      }
    }
  }

  // Gracefully shut down the cluster connection and release resources
  cluster.close().get();

  return 0;
}

auto
program_config::from_env() -> program_config
{
  program_config config{};

  // Override defaults with environment variables when present
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
  if (const auto* val = getenv("COLLECTION_NAME"); val != nullptr) {
    config.collection_name = val;
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
  std::cout << "    COLLECTION_NAME: " << quote(collection_name) << "\n";
  std::cout << "            VERBOSE: " << std::boolalpha << verbose << "\n";
  std::cout << "            PROFILE: " << (profile ? quote(*profile) : "[NONE]") << "\n\n";
}
