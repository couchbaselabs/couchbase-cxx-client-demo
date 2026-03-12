#include <couchbase/cluster.hxx>
#include <couchbase/codec/raw_binary_transcoder.hxx>
#include <couchbase/codec/raw_json_transcoder.hxx>
#include <couchbase/codec/raw_string_transcoder.hxx>
#include <couchbase/logger.hxx>

#include <iomanip>
#include <iostream>

struct program_config {
  std::string connection_string{ "couchbase://127.0.0.1" };
  std::string user_name{ "Administrator" };
  std::string password{ "password" };
  std::string bucket_name{ "default" };
  std::string scope_name{ couchbase::scope::default_name };
  std::string collection_name{ couchbase::collection::default_name };
  std::optional<std::string> profile{};
  bool verbose{ false };

  static auto from_env() -> program_config;
  static auto quote(std::string val) -> std::string;
  void dump();
};

struct passthrough_transcoder {
  using document_type = couchbase::codec::encoded_value;

  static auto encode(const couchbase::codec::encoded_value& document)
    -> couchbase::codec::encoded_value
  {
    return document;
  }

  static auto decode(const couchbase::codec::encoded_value& encoded) -> document_type
  {
    return encoded;
  }
};

template<>
struct couchbase::codec::is_transcoder<passthrough_transcoder> : public std::true_type {
};

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
  } else {
    auto collection = cluster.bucket(config.bucket_name)
                        .scope(config.scope_name)
                        .collection(config.collection_name);

    constexpr char basic_doc[]{ R"({"a": 1.0, "b": 42})" };

    {
      std::cout << "====== Upsert as pre-encoded JSON ======\n";
      const std::string document_id{ "pre_encoded_json" };
      {
        auto [err, resp] = collection
                             .upsert<couchbase::codec::raw_json_transcoder>(
                               document_id, std::string{ basic_doc }, {})
                             .get();
        std::cout << "UPSERT id: \"" << document_id << "\"";
        if (err.ec()) {
          std::cout << ", Error: " << err.message() << "\n";
        } else {
          std::cout << ", CAS: " << resp.cas().value() << "\n";
        }
      }
      {
        auto [err, resp] = collection.get(document_id, {}).get();
        std::cout << "   GET id: \"" << document_id << "\"";
        if (err.ec()) {
          std::cout << ", Error: " << err.message() << "\n";
        } else {
          std::cout << ", CAS: " << resp.cas().value();
          auto document = resp.content_as<passthrough_transcoder>();
          std::cout << ", Flags: 0x" << std::hex << std::setfill('0') << std::setw(8)
                    << document.flags << "\n";
          std::cout << std::string{ document.data.begin(), document.data.end() } << "\n";
        }
      }
    }
    {
      std::cout << "====== Upsert as STRING ======\n";
      const std::string document_id{ "string" };
      {
        auto [err, resp] = collection
                             .upsert<couchbase::codec::raw_string_transcoder>(
                               document_id, std::string{ basic_doc }, {})
                             .get();
        std::cout << "UPSERT id: \"" << document_id << "\"";
        if (err.ec()) {
          std::cout << ", Error: " << err.message() << "\n";
        } else {
          std::cout << ", CAS: " << resp.cas().value() << "\n";
        }
      }
      {
        auto [err, resp] = collection.get(document_id, {}).get();
        std::cout << "   GET id: \"" << document_id << "\"";
        if (err.ec()) {
          std::cout << ", Error: " << err.message() << "\n";
        } else {
          std::cout << ", CAS: " << resp.cas().value();
          auto document = resp.content_as<passthrough_transcoder>();
          std::cout << ", Flags: 0x" << std::hex << std::setfill('0') << std::setw(8)
                    << document.flags << "\n";
          std::cout << std::string{ document.data.begin(), document.data.end() } << "\n";
        }
      }
    }
    {
      std::cout << "====== Upsert as pre-encoded BINARY ======\n";
      const std::string document_id{ "binary" };
      {
        auto [err, resp] =
          collection
            .upsert<couchbase::codec::raw_binary_transcoder>(
              document_id,
              std::vector<std::byte>{ reinterpret_cast<const std::byte*>(basic_doc),
                                      reinterpret_cast<const std::byte*>(basic_doc) +
                                        sizeof(basic_doc) },
              {})
            .get();
        std::cout << "UPSERT id: \"" << document_id << "\"";
        if (err.ec()) {
          std::cout << ", Error: " << err.message() << "\n";
        } else {
          std::cout << ", CAS: " << resp.cas().value() << "\n";
        }
      }
      {
        auto [err, resp] = collection.get(document_id, {}).get();
        std::cout << "   GET id: \"" << document_id << "\"";
        if (err.ec()) {
          std::cout << ", Error: " << err.message() << "\n";
        } else {
          std::cout << ", CAS: " << resp.cas().value();
          auto document = resp.content_as<passthrough_transcoder>();
          std::cout << ", Flags: 0x" << std::hex << std::setfill('0') << std::setw(8)
                    << document.flags << "\n";
          std::cout << std::string{ document.data.begin(), document.data.end() } << "\n";
        }
      }
    }
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
