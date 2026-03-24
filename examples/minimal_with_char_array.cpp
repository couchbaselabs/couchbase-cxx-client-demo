#include <couchbase/cluster.hxx>
#include <couchbase/codec/raw_binary_transcoder.hxx> // Stores bytes with binary flags (0x02000000)
#include <couchbase/codec/raw_json_transcoder.hxx> // Stores a pre-encoded JSON string as-is (0x02000001)
#include <couchbase/codec/raw_string_transcoder.hxx> // Stores a plain string with string flags (0x04000000)
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

// A custom transcoder that bypasses all encoding/decoding and returns the raw
// encoded_value (bytes + flags) exactly as stored on the server. Useful for
// inspecting what the SDK actually wrote — particularly the flags field, which
// tells you how Couchbase classified the document (JSON, string, binary, etc.).
struct passthrough_transcoder {
  using document_type = couchbase::codec::encoded_value;

  static auto encode(const couchbase::codec::encoded_value& document)
    -> couchbase::codec::encoded_value
  {
    return document; // No transformation; send the value exactly as provided
  }

  static auto decode(const couchbase::codec::encoded_value& encoded) -> document_type
  {
    return encoded; // No transformation; expose raw bytes and flags to the caller
  }
};

// Register passthrough_transcoder as a valid transcoder so the SDK accepts it
// in upsert/get calls. Without this trait, the SDK will reject it at compile time.
template<>
struct couchbase::codec::is_transcoder<passthrough_transcoder> : public std::true_type {
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
    auto collection = cluster.bucket(config.bucket_name)
                        .scope(config.scope_name)
                        .collection(config.collection_name);

    // The same raw JSON text is used across all three upserts to highlight
    // how the choice of transcoder affects the flags stored with the document.
    constexpr char basic_doc[]{ R"({"a": 1.0, "b": 42})" };

    {
      // Technique 1: raw_json_transcoder
      // Use this when you already have a serialized JSON string and want
      // Couchbase to treat it as JSON (flags = 0x02000001). The SDK will NOT
      // re-serialize the string — it is written to the server verbatim.
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
          // passthrough_transcoder lets us inspect the raw flags set by raw_json_transcoder
          auto document = resp.content_as<passthrough_transcoder>();
          std::cout << ", Flags: 0x" << std::hex << std::setfill('0') << std::setw(8)
                    << document.flags << "\n"; // Expect 0x02000001 (JSON)
          std::cout << std::string{ document.data.begin(), document.data.end() } << "\n";
        }
      }
    }
    {
      // Technique 2: raw_string_transcoder
      // Use this to store arbitrary text that is NOT JSON (flags = 0x04000000).
      // SDKs in other languages will decode it as a plain string, not a JSON value.
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
          // passthrough_transcoder reveals the string flags set by raw_string_transcoder
          auto document = resp.content_as<passthrough_transcoder>();
          std::cout << ", Flags: 0x" << std::hex << std::setfill('0') << std::setw(8)
                    << document.flags << "\n"; // Expect 0x04000000 (string)
          std::cout << std::string{ document.data.begin(), document.data.end() } << "\n";
        }
      }
    }
    {
      // Technique 3: raw_binary_transcoder
      // Use this for opaque byte payloads (images, Protobuf, etc.) that should
      // not be interpreted as text or JSON (flags = 0x02000000). The SDK accepts
      // a std::vector<std::byte>; here the char array is reinterpret_cast'd into bytes.
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
          // passthrough_transcoder reveals the binary flags set by raw_binary_transcoder
          auto document = resp.content_as<passthrough_transcoder>();
          std::cout << ", Flags: 0x" << std::hex << std::setfill('0') << std::setw(8)
                    << document.flags << "\n"; // Expect 0x02000000 (binary)
          std::cout << std::string{ document.data.begin(), document.data.end() } << "\n";
        }
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
