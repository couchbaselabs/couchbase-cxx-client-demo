#include <couchbase/cluster.hxx>
#include <couchbase/codec/codec_flags.hxx> // Constants for Couchbase common flags (JSON/string/binary)
#include <couchbase/codec/transcoder_traits.hxx> // is_transcoder<T> trait: registers a custom transcoder
#include <couchbase/durability_level.hxx>        // Controls replication guarantees before ack
#include <couchbase/logger.hxx>
#include <couchbase/transactions/attempt_context.hxx> // Transaction handle for synchronous transactions
// async_attempt_context used for callback-style below

#include <fmt/format.h> // {fmt} library used in place of iostream throughout this example
#include <tao/json.hpp>
#include <tao/json/to_string.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <iterator>

#include <arpa/inet.h>

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

// Output iterator that appends chars as std::byte into a vector<std::byte>.
// Used by fmt::format_to so we can build the CSV payload directly into the
// byte buffer that the SDK expects, with no intermediate string copy.
class byte_appender
{
public:
  using iterator_category = std::output_iterator_tag;
  using value_type = void;

  explicit byte_appender(std::vector<std::byte>& output)
    : output_{ output }
  {
  }

  constexpr byte_appender(const byte_appender&) = default;
  byte_appender(byte_appender&&) = default;
  auto operator=(const byte_appender& other) -> byte_appender&
  {
    output_ = other.output_;
    return *this;
  }

  // Called by fmt for each character; cast char → std::byte and append
  auto operator=(char ch) -> byte_appender&
  {
    output_.push_back(static_cast<std::byte>(ch));
    return *this;
  }

  // Required no-ops for the OutputIterator concept
  auto operator*() -> byte_appender&
  {
    return *this;
  }

  auto operator++() const -> byte_appender
  {
    return *this;
  }

  auto operator++(int) const -> byte_appender
  {
    return *this;
  }

private:
  std::vector<std::byte>& output_;
};

// Tell {fmt} that byte_appender satisfies the OutputIterator<char> concept
// so fmt::format_to accepts it as a destination.
template<>
struct fmt::detail::is_output_iterator<byte_appender, char> : std::true_type {
};

// A single row in a double-entry bookkeeping ledger.
// Each transfer generates two entries: one debit and one credit.
struct ledger_entry {
  std::string date{};
  std::string description{};
  std::string account{};
  std::uint64_t debit{};
  std::uint64_t credit{};
};

// In-memory ledger: holds a collection of double-entry bookkeeping rows and
// knows how to serialize/deserialize itself as CSV. The CSV wire format is
// intentionally simple — just a header row followed by one row per entry —
// to show that *any* encoding can be plugged into the SDK via a custom transcoder.
class ledger
{
public:
  // Record a transfer of `amount` from `from_account` to `to_account`.
  // Double-entry bookkeeping requires two rows per transfer:
  //   1. A debit on the receiving account
  //   2. A credit on the sending account
  void add_record(const std::string& date,
                  const std::string& from_account,
                  const std::string& to_account,
                  std::uint64_t amount,
                  const std::string& description)
  {
    entries_.push_back({
      date,
      description,
      to_account,
      /* debit  */ amount,
      /* credit */ 0,
    });
    entries_.push_back({
      date,
      description,
      from_account,
      /* debit  */ 0,
      /* credit */ amount,
    });
  }

  [[nodiscard]] auto entries() const -> const std::vector<ledger_entry>&
  {
    return entries_;
  }

  // Serialize the ledger to CSV bytes suitable for storing in Couchbase.
  // Uses byte_appender so fmt writes directly into the output vector.
  [[nodiscard]] auto to_csv() const -> std::vector<std::byte>
  {
    std::vector<std::byte> buffer;
    byte_appender output(buffer);

    fmt::format_to(output, "Date,Description,Account,Debit,Credit\n");
    for (const auto& entry : entries_) {
      fmt::format_to(output,
                     "{},{},{},{},{}\n",
                     entry.date,
                     entry.description,
                     entry.account,
                     entry.debit,
                     entry.credit);
    }
    return buffer;
  }

  // Deserialize a ledger from raw CSV bytes retrieved from Couchbase.
  // Skips the header row, then parses each subsequent line field-by-field.
  static auto from_csv(const std::vector<std::byte>& blob) -> ledger
  {

    ledger ret;

    std::istringstream input({
      reinterpret_cast<const char*>(blob.data()),
      blob.size(),
    });
    std::string line;

    bool header_line{ true };
    while (std::getline(input, line)) {
      if (header_line) {
        header_line = false; // Discard the "Date,Description,Account,Debit,Credit" header
        continue;
      }
      std::istringstream line_stream(line);

      ledger_entry entry;

      std::getline(line_stream, entry.date, ',');
      std::getline(line_stream, entry.description, ',');
      std::getline(line_stream, entry.account, ',');

      std::string field;
      std::getline(line_stream, field, ',');
      if (!field.empty()) {
        entry.debit = std::stoul(field);
      }
      std::getline(line_stream, field, ',');
      if (!field.empty()) {
        entry.credit = std::stoul(field);
      }

      ret.entries_.push_back(entry);
    }
    return ret;
  }

  // Pretty-print the ledger as a fixed-width table for console output
  [[nodiscard]] auto to_string() const -> std::string
  {
    std::string buffer;
    auto output = std::back_inserter(buffer);

    fmt::format_to(output,
                   "{:<15} {:<30} {:<20} {:>10} {:>10}\n{:-<90}\n",
                   "Date",
                   "Description",
                   "Account",
                   "Debit",
                   "Credit",
                   "");

    for (const auto& entry : entries_) {
      fmt::format_to(output,
                     "{:<15} {:<30} {:<20} {:>10} {:>10}\n",
                     entry.date,
                     entry.description,
                     entry.account,
                     entry.debit,
                     entry.credit);
    }

    return buffer;
  }

private:
  std::vector<ledger_entry> entries_{};
};

// Custom transcoder that bridges between the `ledger` domain type and
// Couchbase's encoded_value (raw bytes + flags). The SDK calls encode()
// on writes and decode() on reads; all call sites just pass `ledger` values.
//
// Because the CSV payload is opaque text rather than JSON, we tag it with
// binary_common_flags so other SDK clients know not to attempt JSON parsing.
struct csv_transcoder {
  using document_type = ledger;

  template<typename Document = document_type>
  static auto encode(const Document& document) -> couchbase::codec::encoded_value
  {
    return {
      document.to_csv(),                                  // Serialize to CSV bytes
      couchbase::codec::codec_flags::binary_common_flags, // Mark as binary, not JSON
    };
  }

  template<typename Document = document_type>
  static auto decode(const couchbase::codec::encoded_value& encoded) -> Document
  {
    // Reject documents stored with incompatible flags (e.g. written by a JSON transcoder)
    if (encoded.flags == 0 &&
        !couchbase::codec::codec_flags::has_common_flags(
          encoded.flags, couchbase::codec::codec_flags::binary_common_flags)) {
      throw std::system_error(
        couchbase::errc::common::decoding_failure,
        "csv_transcoder expects document to have binary common flags, flags=" +
          std::to_string(encoded.flags));
    }

    return Document::from_csv(encoded.data); // Deserialize CSV bytes back into a ledger
  }
};

// Register csv_transcoder with the SDK at compile time.
// Without this trait specialization, passing csv_transcoder to upsert/get/replace
// will produce a static_assert failure.
template<>
struct couchbase::codec::is_transcoder<csv_transcoder> : public std::true_type {
};

auto
main(int argc, const char* argv[]) -> int
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
    fmt::println(stderr, "Unable to connect to the cluster. ec: {}", connect_err.message());
    return EXIT_FAILURE;
  }

  auto collection =
    cluster.bucket(config.bucket_name).scope(config.scope_name).collection(config.collection_name);

  // Represent a ledger that tracks fund movements between accounts using CSV encoding.
  // Couchbase is agnostic to the encoding; we simply tag the payload as binary and
  // provide csv_transcoder to handle serialization. This pattern works identically
  // with Protobuf, MessagePack, Avro, or any other encoding — just swap the transcoder.
  //
  // This is how the ledger might look like at some point in time:
  //
  // Date,Description,Account,Debit,Credit
  // 2024-08-30,Payment received,Cash,1500,0
  // 2024-08-30,Payment received,Accounts Receivable,0,1500
  // 2024-08-31,Rent payment,Expenses,1000,0
  // 2024-08-31,Rent payment,Cash,0,1000
  // 2024-09-01,Office Supplies,Expenses,200,0
  // 2024-09-01,Office Supplies,Cash,0,200
  // 2024-09-02,Client Invoice,Accounts Receivable,1200,0
  // 2024-09-02,Client Invoice,Revenue,0,1200
  //
  // The application must inform the SDK that this is a "binary" (as a opposed
  // to "JSON") data, and provide custom transcoder to ensure that the SDK will
  // handle everything correctly.
  ledger initial_state;
  initial_state.add_record("2024-08-30", "Accounts Receivable", "Cash", 1500, "Payment received");

  // majority durability: acknowledged only after a majority of replicas have the write in memory
  auto upsert_options =
    couchbase::upsert_options{}.durability(couchbase::durability_level::majority);
  // Explicitly supply both the transcoder and the document type; the SDK encodes via csv_transcoder
  auto [err, res] =
    collection.upsert<csv_transcoder, ledger>("the_ledger", initial_state, upsert_options).get();
  if (err.ec()) {
    fmt::println(
      stderr,
      "Create initial state of \"the_ledger\" has failed before starting transaction: {}",
      err.ec().message());
    return EXIT_FAILURE;
  }

  {
    // Synchronous transaction: appends a "Rent payment" entry to the ledger atomically.
    // The SDK automatically retries on transient conflicts.
    auto [tx_err, tx_res] = cluster.transactions()->run(
      [=](std::shared_ptr<couchbase::transactions::attempt_context> ctx) -> couchbase::error {
        auto [err_ctx, doc] = ctx->get(collection, "the_ledger");
        if (err_ctx.ec()) {
          fmt::println(stderr, "Failed to retrieve \"the_ledger\": {}", err_ctx.ec().message());
          return {};
        }

        // Decode the raw CSV bytes into a ledger struct using csv_transcoder::decode()
        auto the_ledger = doc.content_as<ledger, csv_transcoder>();
        the_ledger.add_record("2024-09-01", "Cash", "Expenses", 1000, "Rent payment");
        // Re-encode and stage the updated ledger; the CSV bytes are written on commit
        ctx->replace<csv_transcoder, ledger>(doc, the_ledger);
        return {};
      });

    if (tx_err.ec()) {
      fmt::println(stderr,
                   "error in transaction {}, cause: {}",
                   tx_err.ec().message(),
                   tx_err.cause().has_value() ? tx_err.cause().value().ec().message() : "");
      return EXIT_FAILURE;
    } else {
      fmt::println("transaction {} completed successfully", tx_res.transaction_id);
    }
  }

  {
    // Asynchronous transaction: same ledger update but using the callback-based API
    // (async_attempt_context). A promise/future pair is used to block main until the
    // transaction completes, bridging the async API into a synchronous flow.
    auto barrier = std::make_shared<std::promise<std::error_code>>();
    auto f = barrier->get_future();
    cluster.transactions()->run(
      // First lambda: the transaction body — receives an async_attempt_context
      [=](std::shared_ptr<couchbase::transactions::async_attempt_context> ctx) -> couchbase::error {
        ctx->get(collection, "the_ledger", [=](auto err_ctx_1, auto doc) {
          if (err_ctx_1.ec()) {
            fmt::println(
              stderr, "failed to get document \"the_ledger\": {}", err_ctx_1.ec().message());
            return;
          }

          // Decode CSV bytes into a ledger struct, then append the new entry
          auto the_ledger = doc.template content_as<ledger, csv_transcoder>();
          the_ledger.add_record("2024-09-01", "Cash", "Expenses", 200, "Office Supplies");

          // Re-encode and stage the updated ledger within the same transaction attempt
          ctx->replace<csv_transcoder, ledger>(
            doc, std::move(the_ledger), [=](auto err_ctx_2, auto /*res*/) {
              if (err_ctx_2.ec()) {
                fmt::println(stderr,
                             "error replacing content in doc \"the_ledger\": {}",
                             err_ctx_2.ec().message());
              } else {
                fmt::println("successfully replaced: \"the_ledger\"");
              }
            });
        });
        return {};
      },

      // Second lambda: completion handler — called once the transaction commits or fails
      [barrier](auto tx_err, auto tx_res) {
        if (tx_err.ec()) {
          fmt::println(stderr,
                       "error in async transaction {}, {}",
                       tx_res.transaction_id,
                       tx_err.ec().message());
        }
        barrier->set_value(tx_err.ec()); // Unblock the future in main
      });
    if (auto async_err = f.get()) {
      fmt::println(stderr, "received async error from future: message - {}", async_err.message());
      return EXIT_FAILURE;
    }
  }

  // Read back the final ledger and pretty-print it to confirm all three entries are present
  {
    auto [err, resp] = collection.get("the_ledger", {}).get();
    if (err.ec()) {
      fmt::println(stderr, "Unable to read \"the_ledger\": {}", err.message());
      return EXIT_FAILURE;
    }
    fmt::println("The final result:\n{}", resp.content_as<ledger, csv_transcoder>().to_string());
  }

  // Gracefully shut down the cluster connection and release resources
  cluster.close().get();
  return EXIT_SUCCESS;
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
  fmt::println("  CONNECTION_STRING: {}", quote(connection_string));
  fmt::println("          USER_NAME: {}", quote(user_name));
  fmt::println("           PASSWORD: [HIDDEN]");
  fmt::println("        BUCKET_NAME: {}", quote(bucket_name));
  fmt::println("         SCOPE_NAME: {}", quote(scope_name));
  fmt::println("    COLLECTION_NAME: {}", quote(collection_name));
  fmt::println("            VERBOSE: {}", verbose);
  fmt::println("            PROFILE: {}", (profile ? quote(*profile) : "[NONE]"));
}
