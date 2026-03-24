#include <couchbase/cluster.hxx>
#include <couchbase/codec/tao_json_serializer.hxx> // JSON serialization for bank_account
#include <couchbase/durability_level.hxx>          // Controls replication guarantees before ack
#include <couchbase/logger.hxx>
#include <couchbase/transactions/attempt_context.hxx> // Transaction handle: get/replace/insert/remove

#include <tao/json.hpp>
#include <tao/json/to_string.hpp>

#include <cstdlib>
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

// Application-level error codes for the banking domain.
// Returning one of these from inside a transaction lambda causes the
// transaction to be rolled back with this code as the cause.
enum bank_error : int {
  insufficient_funds = 1,
};

namespace
{
// std::error_category subclass that gives bank_error codes human-readable names.
// Required boilerplate to integrate a custom enum with std::error_code.
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

// Singleton category instance — error_category objects must outlive any error_code that uses them.
const static bank_error_category instance{};
const std::error_category&
bank_error_category_instance() noexcept
{
  return instance;
}
} // namespace

// Tell the standard library that bank_error values can be implicitly converted to std::error_code.
template<>
struct std::is_error_code_enum<bank_error> : std::true_type {
};

// ADL-found factory that constructs a std::error_code from a bank_error value.
auto
make_error_code(bank_error e) -> std::error_code
{
  return { static_cast<int>(e), bank_error_category_instance() };
}

// Simple ledger entry: a named account with a balance in USD cents (or whole dollars here).
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

// Teach tao/json how to serialize and deserialize bank_account.
// assign() converts C++ → JSON; as() converts JSON → C++.
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
    return EXIT_FAILURE;
  }

  auto collection =
    cluster.bucket(config.bucket_name).scope(config.scope_name).collection(config.collection_name);

  // majority durability: the write is acknowledged only after a majority of replicas
  // have it in memory. Using this for the seed upserts matches the durability guarantee
  // that Couchbase Transactions enforce internally, avoiding inconsistent initial state.
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
    // cluster.transactions()->run() executes the lambda as a single ACID transaction.
    // If the lambda returns an error — or if a transient conflict occurs — the SDK
    // automatically retries with exponential back-off. All ctx->get/replace calls
    // inside the lambda are part of the same atomic unit of work.
    auto [err, res] = cluster.transactions()->run(
      [collection](
        std::shared_ptr<couchbase::transactions::attempt_context> ctx) -> couchbase::error {
        // ctx->get locks the document for the duration of this attempt
        auto [e1, alice] = ctx->get(collection, "alice");
        if (e1.ec()) {
          std::cout << "Unable to read account for Alice: " << e1.ec().message() << "\n";
          return e1; // Returning an error rolls back and stops retrying
        }
        auto alice_content = alice.content_as<bank_account>();

        auto [e2, bob] = ctx->get(collection, "bob");
        if (e2.ec()) {
          std::cout << "Unable to read account for Bob: " << e2.ec().message() << "\n";
          return e2;
        }
        auto bob_content = bob.content_as<bank_account>();

        const std::int64_t money_to_transfer = 1'234;
        if (alice_content.balance < money_to_transfer) {
          std::cout << "Alice does not have enough money to transfer " << money_to_transfer
                    << " USD to Bob\n";
          // Returning an application error causes an immediate rollback — no retry
          return {
            bank_error::insufficient_funds,
            "not enough funds on Alice's account",
          };
        }
        // Debit Alice and credit Bob atomically — both writes commit or neither does
        alice_content.balance -= money_to_transfer;
        bob_content.balance += money_to_transfer;

        {
          // ctx->replace stages the write; it is not visible outside the transaction until commit
          auto [e3, a] = ctx->replace(alice, alice_content);
          if (e3.ec()) {
            std::cout << "Unable to read account for Alice: " << e3.ec().message() << "\n";
          }
        }
        {
          auto [e4, b] = ctx->replace(bob, bob_content);
          if (e4.ec()) {
            std::cout << "Unable to update account for Bob: " << e4.ec().message() << "\n";
          }
        }
        return {}; // Empty error signals success; the SDK commits the transaction
      });

    if (err.ec()) {
      std::cout << "Transaction has failed: " << err.ec().message() << "\n";
      // err.cause() carries the underlying application or SDK error that triggered the failure
      if (auto cause = err.cause(); cause.has_value()) {
        std::cout << "Cause: " << cause->ec().message() << "\n";
      }
      return EXIT_FAILURE;
    }
  }

  // Read back both accounts to confirm the transfer is visible post-commit
  {
    auto [err, resp] = collection.get("alice", {}).get();
    if (err.ec()) {
      std::cout << "Unable to update account for Alice: " << err.message() << "\n";
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
