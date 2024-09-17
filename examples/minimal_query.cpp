#include <couchbase/cluster.hxx>
#include <couchbase/codec/tao_json_serializer.hxx>
#include <couchbase/logger.hxx>

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

struct airline {
  std::uint32_t id{ 0 };
  std::string name;
  std::string iata;
  std::string icao;
  std::string callsign;
  std::string country;
};

std::ostream&
operator<<(std::ostream& os, const airline& a)
{
  os << "airline(id: " << a.id << ", name: \"" << a.name << "\", iata: \"" << a.iata
     << "\", icao: \"" << a.icao << "\", callsign: \"" << a.callsign << "\", country: \""
     << a.country << "\")";
  return os;
}

template<>
struct tao::json::traits<airline> {
  template<template<typename...> class Traits>
  static airline as(const tao::json::basic_value<Traits>& v)
  {
    if (!v.is_object()) {
      return {};
    }
    if (const auto* airline_json = v.find("airline");
        airline_json != nullptr && airline_json->is_object()) {
      airline result{};
      const auto& object = airline_json->get_object();
      result.id = object.at("id").template optional<std::uint32_t>().value_or(0);
      result.name = object.at("name").template optional<std::string>().value_or("");
      result.iata = object.at("iata").template optional<std::string>().value_or("");
      result.icao = object.at("icao").template optional<std::string>().value_or("");
      result.callsign = object.at("callsign").template optional<std::string>().value_or("");
      result.country = object.at("country").template optional<std::string>().value_or("");
      return result;
    }
    return {};
  }
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
    return EXIT_FAILURE;
  }
  auto scope = cluster.bucket(config.bucket_name).scope(config.scope_name);

  auto [err, resp] = scope.query("SELECT * FROM airline LIMIT 10").get();

  std::cout << "--- Iterating as Binary data:\n";
  for (auto row : resp.rows_as_binary()) {
    std::cout << std::string(reinterpret_cast<const char*>(row.data()), row.size()) << "\n";
  }

  std::cout << "--- Iterating as C++ types:\n";
  for (auto row : resp.rows_as<couchbase::codec::tao_json_serializer, airline>()) {
    std::cout << row << "\n";
  }

  std::cout << "--- Iterating as JSON objects:\n";
  for (auto row : resp.rows_as()) {
    std::cout << "Airline(id: " << row["airline"]["id"] << ", name: \"" << row["airline"]["name"]
              << "\")\n";
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
