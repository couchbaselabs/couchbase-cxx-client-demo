#include "couchbase/durability_level.hxx"
#include "couchbase/transactions/attempt_context.hxx"
#include <couchbase/cluster.hxx>
#include <couchbase/logger.hxx>

#include <tao/json.hpp>
#include <tao/json/to_string.hpp>

#include <cstdlib>
#include <iostream>

struct program_config {
  std::string connection_string{ "couchbase://127.0.0.1" };
  std::string user_name{ "Administrator" };
  std::string password{ "password" };
  std::string preferred_server_group{ "Group 1" };
  std::string bucket_name{ "default" };
  std::string scope_name{ couchbase::scope::default_name };
  std::string collection_name{ couchbase::collection::default_name };
  std::optional<std::string> profile{};
  bool verbose{ false };

  static auto from_env() -> program_config;
  static auto quote(std::string val) -> std::string;
  void dump();
};

enum bank_error : int {
  insufficient_funds = 1,
};

namespace
{
struct bank_error_category : std::error_category {
  [[nodiscard]] auto name() const noexcept -> const char* override
  {
    return "bank_error";
  }

  [[nodiscard]] auto message(int ev) const noexcept -> std::string override
  {
    switch (static_cast<bank_error>(ev)) {
      case insufficient_funds:
        return "insufficient_funds (1): not enough funds on the account";
        break;
    }
    return "unexpected error code in \"bank_error\" category, ev=" + std::to_string(ev);
  }
};

const static bank_error_category instance{};
const std::error_category&
bank_error_category_instance() noexcept
{
  return instance;
}
} // namespace

template<>
struct std::is_error_code_enum<bank_error> : std::true_type {
};

auto
make_error_code(bank_error e) -> std::error_code
{
  return { static_cast<int>(e), bank_error_category_instance() };
}

struct bank_account {
  std::string name;
  std::int64_t balance;
};

std::ostream&
operator<<(std::ostream& os, const bank_account& a)
{
  os << "bank_account(name: \"" << a.name << "\", balance: " << a.balance << " USD)";
  return os;
}

template<>
struct tao::json::traits<bank_account> {
  template<template<typename...> class Traits>
  static void assign(tao::json::basic_value<Traits>& v, const bank_account& p)
  {
    v = {
      { "name", p.name },
      { "balance", p.balance },
    };
  }

  template<template<typename...> class Traits>
  static bank_account as(const tao::json::basic_value<Traits>& v)
  {
    bank_account result;
    const auto& object = v.get_object();
    result.name = object.at("name").template as<std::string>();
    result.balance = object.at("balance").template as<std::int64_t>();
    return result;
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
  options.network().preferred_server_group(config.preferred_server_group);
  if (config.profile) {
    options.apply_profile(config.profile.value());
  }

  auto [connect_err, cluster] =
    couchbase::cluster::connect(config.connection_string, options).get();
  if (connect_err) {
    std::cout << "Unable to connect to the cluster. ec: " << connect_err.message() << "\n";
    return EXIT_FAILURE;
  }

  auto collection =
    cluster.bucket(config.bucket_name).scope(config.scope_name).collection(config.collection_name);

  auto upsert_options =
    couchbase::upsert_options{}.durability(couchbase::durability_level::majority);
  {
    bank_account alice{ "Alice", 124'000 };
    std::cout << "Initialize account for Alice: " << alice << "\n";
    auto [err, resp] = collection.upsert("alice", alice, upsert_options).get();
    if (err.ec()) {
      std::cout << "Unable to create an account for Alice: " << err.message() << "\n";
      return EXIT_FAILURE;
    }
    std::cout << "Stored account for Alice (CAS=" << resp.cas().value() << ")\n";
  }
  {
    bank_account bob{ "Bob", 42'000 };
    std::cout << "Initialize account for Bob: " << bob << "\n";
    auto [err, resp] = collection.upsert("bob", bob, upsert_options).get();
    if (err.ec()) {
      std::cout << "Unable to create an account for Bob: " << err.message() << "\n";
      return EXIT_FAILURE;
    }
    std::cout << "Stored account for Bob (CAS=" << resp.cas().value() << ")\n";
  }

  {
    auto [err, res] = cluster.transactions()->run(
      [collection, preferred_server_group = config.preferred_server_group](
        std::shared_ptr<couchbase::transactions::attempt_context> ctx) -> couchbase::error {
        auto [e1, alice] = ctx->get_replica_from_preferred_server_group(collection, "alice");
        if (e1.ec()) {
          std::cout << "Unable to read account for Alice from preferred group \""
                    << preferred_server_group << "\": " << e1.ec().message()
                    << ". Falling back to regular get\n";
          auto [e2, alice_fallback] = ctx->get(collection, "alice");
          if (e2.ec()) {
            std::cout << "Unable to read account for Alice: " << e2.ec().message() << "\n";
            return e2;
          }
          alice = alice_fallback;
        }
        auto alice_content = alice.content_as<bank_account>();

        auto [e3, bob] = ctx->get_replica_from_preferred_server_group(collection, "bob");
        if (e3.ec()) {
          std::cout << "Unable to read account for Bob from preferred group \""
                    << preferred_server_group << "\": " << e3.ec().message()
                    << ". Falling back to regular get\n";
          auto [e4, bob_fallback] = ctx->get(collection, "bob");
          if (e4.ec()) {
            std::cout << "Unable to read account for Alice: " << e4.ec().message() << "\n";
            return e4;
          }
          bob = bob_fallback;
        }
        auto bob_content = bob.content_as<bank_account>();

        const std::int64_t money_to_transfer = 1'234;
        if (alice_content.balance < money_to_transfer) {
          std::cout << "Alice does not have enough money to transfer " << money_to_transfer
                    << " USD to Bob\n";
          return {
            bank_error::insufficient_funds,
            "not enough funds on Alice's account",
          };
        }
        alice_content.balance -= money_to_transfer;
        bob_content.balance += money_to_transfer;

        {
          auto [e5, a] = ctx->replace(alice, alice_content);
          if (e5.ec()) {
            std::cout << "Unable to update account for Alice: " << e5.ec().message() << "\n";
          }
        }
        {
          auto [e6, b] = ctx->replace(bob, bob_content);
          if (e6.ec()) {
            std::cout << "Unable to update account for Bob: " << e6.ec().message() << "\n";
          }
        }
        return {};
      });

    if (err.ec()) {
      std::cout << "Transaction has failed: " << err.ec().message() << "\n";
      if (auto cause = err.cause(); cause.has_value()) {
        std::cout << "Cause: " << cause->ec().message() << "\n";
      }
      return EXIT_FAILURE;
    }
  }

  {
    auto [err, resp] = collection.get("alice", {}).get();
    if (err.ec()) {
      std::cout << "Unable to read account for Alice: " << err.message() << "\n";
      return EXIT_FAILURE;
    }
    std::cout << "Alice (CAS=" << resp.cas().value() << "): " << resp.content_as<bank_account>()
              << "\n";
  }
  {
    auto [err, resp] = collection.get("bob", {}).get();
    if (err.ec()) {
      std::cout << "Unable to read account for Bob: " << err.message() << "\n";
      return EXIT_FAILURE;
    }
    std::cout << "Bob (CAS=" << resp.cas().value() << "): " << resp.content_as<bank_account>()
              << "\n";
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
  if (const auto* val = getenv("PREFERRED_SERVER_GROUP"); val != nullptr) {
    config.preferred_server_group = val;
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
  std::cout << "       CONNECTION_STRING: " << quote(connection_string) << "\n";
  std::cout << "               USER_NAME: " << quote(user_name) << "\n";
  std::cout << "                PASSWORD: [HIDDEN]\n";
  std::cout << "  PREFERRED_SERVER_GROUP: " << quote(preferred_server_group) << "\n";
  std::cout << "             BUCKET_NAME: " << quote(bucket_name) << "\n";
  std::cout << "              SCOPE_NAME: " << quote(scope_name) << "\n";
  std::cout << "         COLLECTION_NAME: " << quote(collection_name) << "\n";
  std::cout << "                 VERBOSE: " << std::boolalpha << verbose << "\n";
  std::cout << "                 PROFILE: " << (profile ? quote(*profile) : "[NONE]") << "\n\n";
}
