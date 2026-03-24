/*
 * inventory_with_opentelemetry — Couchbase C++ SDK + OpenTelemetry example
 *
 * Demonstrates how to instrument a Couchbase C++ application with OpenTelemetry
 * distributed tracing and metrics, and how to ship both signals to the bundled
 * observability stack in ./telemetry-cluster.
 *
 * ============================================================================
 * OpenTelemetry integration with the Couchbase C++ SDK
 * ============================================================================
 *
 * The SDK exposes two hook points in couchbase::cluster_options:
 *
 *   Tracing  (couchbase::tracing::otel_request_tracer)
 *     Wraps an opentelemetry::trace::Tracer.  Installed via:
 *       options.tracing().tracer(std::make_shared<otel_request_tracer>(tracer))
 *     Every SDK operation — upsert, get, query, etc. — creates a child span
 *     under the parent_span supplied at call time (e.g., upsert_options{}
 *     .parent_span(cb_parent)).  Child spans are annotated with the bucket,
 *     scope, collection, and internal timing (encode / dispatch / decode).
 *     See apply_opentelemetry_tracer_options() below.
 *
 *   Metrics  (couchbase::metrics::otel_meter)
 *     Wraps an opentelemetry::metrics::Meter.  Installed via:
 *       options.metrics().meter(std::make_shared<otel_meter>(meter))
 *     The SDK records per-operation latency histograms (db.client.operation.duration,
 *     unit "s") and retry/timeout counters, all labelled by bucket, scope, collection,
 *     and operation type.  A PeriodicExportingMetricReader (default interval: 5 s)
 *     pushes those measurements to the configured OTLP endpoint.
 *
 *     Histogram bucket calibration: the SDK measures durations in microseconds
 *     internally, then converts to seconds before recording.  The OpenTelemetry
 *     SDK's built-in default histogram boundaries are calibrated for millisecond
 *     values and are therefore orders of magnitude too large for second-valued
 *     Couchbase metrics; almost every sample would fall into the first bucket,
 *     rendering percentile estimates meaningless.  apply_opentelemetry_meter_options()
 *     installs a process-wide catch-all View that replaces those defaults with eight
 *     boundaries spanning 100 µs to 10 s — the second-scale equivalent of the
 *     Couchbase Java SDK's canonical nanosecond boundaries.  See
 *     opentelemetry_metrics_config::histogram_boundaries for the full conversion table.
 *
 *     See apply_opentelemetry_meter_options() below.
 *
 * Both providers use an AlwaysOnSampler / cumulative aggregation and export
 * via OTLP/HTTP JSON to the OTel Collector.  ForceFlush is called explicitly
 * before exit so no spans or metrics are dropped.
 *
 * NOTE: AlwaysOnSampler (100 % sampling) is fine for demos and development but
 * is rarely appropriate in production, where it can generate significant traffic
 * and storage costs.  Choose a sampler that fits your application and
 * infrastructure — for example, ParentBased(TraceIdRatioBased(N)) for
 * head-based probabilistic sampling or a tail-based sampler in the Collector.
 *
 * NOTE: The Couchbase C++ SDK currently supports only metrics and traces.
 * It does not emit logs via OpenTelemetry.  The Loki and Promtail containers
 * in the telemetry-cluster stack are present for completeness but receive no
 * data from this example.
 *
 * ============================================================================
 * Signal flow through the telemetry-cluster stack
 * ============================================================================
 *
 *   This program
 *     │  OTLP/HTTP  http://localhost:4318/v1/traces    (traces)
 *     │  OTLP/HTTP  http://localhost:4318/v1/metrics   (metrics)
 *     ▼
 *   OpenTelemetry Collector  (telemetry-cluster/otel-collector-config.yaml)
 *     │  traces  ── OTLP/gRPC ──► Jaeger                         (port 16686)
 *     │  metrics ── Prometheus scrape endpoint :8889 ──► Prometheus (port 9090)
 *     ▼
 *   Jaeger      http://localhost:16686 — distributed trace viewer
 *   Prometheus  http://localhost:9090  — time-series metrics store
 *   Grafana     http://localhost:3000  — unified dashboards (queries both)
 *
 * ============================================================================
 * Quick-start: Linux + docker compose
 * ============================================================================
 *
 * 1. Start the observability stack:
 *
 *      cd telemetry-cluster
 *      docker compose up -d
 *
 *    Containers started: otel-collector, jaeger, prometheus, loki,
 *    promtail, grafana.  Allow ~10 s for all services to become healthy.
 *    (Loki and Promtail are unused by this example — the C++ SDK does not
 *    emit logs via OpenTelemetry.)
 *
 * 2. Build the example (from the repo root):
 *
 *      cmake -B build -DCMAKE_BUILD_TYPE=Release
 *      cmake --build build --target inventory_with_opentelemetry
 *
 * 3. Run the example (required env vars shown; all others have defaults):
 *
 *      CONNECTION_STRING=couchbase://127.0.0.1 \
 *      USER_NAME=Administrator \
 *      PASSWORD=password \
 *      BUCKET_NAME=default \
 *        ./build/examples/inventory_with_opentelemetry
 *
 *    The OTLP endpoints default to http://localhost:4318/v1/{traces,metrics},
 *    which points at the OTel Collector started in step 1.  Override with:
 *      OTEL_TRACES_ENDPOINT=http://localhost:4318/v1/traces
 *      OTEL_METRICS_ENDPOINT=http://localhost:4318/v1/metrics
 *
 *    Diagnostic flags:
 *      OTEL_VERBOSE=true  — print OTel SDK internal warnings/errors to stderr
 *      VERBOSE=true       — enable Couchbase SDK trace-level logging to stderr
 *
 * ============================================================================
 * Where to see the generated traces and metrics
 * ============================================================================
 *
 * Traces → Jaeger UI  http://localhost:16686
 *   1. Open the Jaeger UI in a browser.
 *   2. In the "Service" drop-down select "inventory-service".
 *   3. Click "Find Traces".
 *   4. Open the "update-inventory" trace.  The hierarchy looks like:
 *        update-inventory                   ← top-level span (this program)
 *          upsert                           ← SDK upsert operation
 *            request_encoding               ← document serialization
 *            dispatch_to_server             ← server round-trip
 *          get                              ← SDK get operation
 *            dispatch_to_server             ← server round-trip
 *      Operation spans (upsert, get) carry: db.system.name, db.namespace,
 *      db.operation.name, couchbase.collection.name, couchbase.scope.name,
 *      couchbase.service, couchbase.retries.
 *      dispatch_to_server spans carry: network.peer.address, network.peer.port,
 *      network.transport, server.address, server.port, couchbase.operation_id,
 *      couchbase.server_duration, couchbase.local_id.
 *
 * Metrics → Prometheus  http://localhost:9090
 *   The OTel Collector exposes a Prometheus scrape endpoint on :8889;
 *   Prometheus scrapes it every 15 s (telemetry-cluster/prometheus.yml).
 *   The Couchbase C++ SDK records a single histogram instrument:
 *     db.client.operation.duration  (unit "s")
 *   which Prometheus renders with the standard histogram suffixes:
 *     db_client_operation_duration_seconds_bucket — per-bucket sample counts (use for percentiles)
 *     db_client_operation_duration_seconds_sum    — cumulative latency across all operations
 *     db_client_operation_duration_seconds_count  — total number of completed operations
 *   Each series is labelled with the service type (kv, query, …) and operation name
 *   (upsert, get, …), allowing fine-grained per-operation latency analysis.
 *
 * Metrics + Traces → Grafana  http://localhost:3000
 *   Grafana is pre-provisioned (anonymous Admin, no login required) with
 *   Prometheus and Jaeger as data sources.
 *   - "Explore → Prometheus": query SDK metrics by name or label.
 *   - "Explore → Jaeger": search traces by service "inventory-service".
 */

#include <couchbase/cluster.hxx>
#include <couchbase/codec/tao_json_serializer.hxx>
#include <couchbase/logger.hxx>

#include <couchbase/tracing/otel_tracer.hxx>

#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/trace/provider.h>

// Metrics SDK
#include <opentelemetry/sdk/metrics/aggregation/aggregation_config.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader.h>
#include <opentelemetry/sdk/metrics/instruments.h>
#include <opentelemetry/sdk/metrics/meter.h>
#include <opentelemetry/sdk/metrics/meter_context_factory.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>
#include <opentelemetry/sdk/metrics/view/instrument_selector_factory.h>
#include <opentelemetry/sdk/metrics/view/meter_selector_factory.h>
#include <opentelemetry/sdk/metrics/view/view_factory.h>
#include <opentelemetry/sdk/metrics/view/view_registry_factory.h>

// Trace SDK
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/samplers/always_on_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>

// Resource (shared between metrics and traces)
#include <opentelemetry/sdk/resource/resource.h>

// OTLP exporters
#include <opentelemetry/exporters/otlp/otlp_http.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h>

#include <couchbase/metrics/otel_meter.hxx>

#include <tao/json.hpp>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace
{
// Diagnostic log handler for the OpenTelemetry SDK.
// Forwards OTel SDK internal messages (export errors, warnings, etc.) to stderr.
// Install via OTEL_VERBOSE=true to surface problems that the SDK would otherwise swallow.
class otel_diagnostic_log_handler : public opentelemetry::sdk::common::internal_log::LogHandler
{
public:
  void Handle(opentelemetry::sdk::common::internal_log::LogLevel level,
              const char* file,
              int line,
              const char* msg,
              const opentelemetry::sdk::common::AttributeMap& attributes) noexcept override
  {
    std::ostringstream oss;
    oss << "OpenTelemetry(" << opentelemetry::sdk::common::internal_log::LevelToString(level)
        << ") [" << (file != nullptr ? file : "?") << ":" << line
        << "]: " << (msg != nullptr ? msg : "");
    if (!attributes.empty()) {
      oss << " {";
      bool first = true;
      for (const auto& [key, val] : attributes) {
        if (!first) {
          oss << ", ";
        }
        oss << key << "=";
        std::visit(
          [&oss](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
              oss << (v ? "true" : "false");
            } else if constexpr (std::is_arithmetic_v<T>) {
              oss << v;
            } else if constexpr (std::is_same_v<T, std::string>) {
              oss << v;
            } else {
              oss << "[" << v.size() << " items]";
            }
          },
          val);
        first = false;
      }
      oss << "}";
    }
    oss << "\n";
    std::cerr << oss.str();
  }
};

const std::array<std::string, 5> truthy_values = {
  "yes", "y", "on", "true", "1",
};

auto
quote(std::string val) -> std::string
{
  return "\"" + val + "\"";
}

auto
quote(std::optional<std::string> val) -> std::string
{
  if (val) {
    return quote(*val);
  }
  return "[NONE]";
}
} // namespace

// Runtime configuration for the OTLP metrics pipeline.
// All fields can be overridden via environment variables; see fill_from_env().
struct opentelemetry_metrics_config {
  // OTLP HTTP endpoint for metrics.
  // Default when unset: http://localhost:4318/v1/metrics (OTel Collector HTTP port).
  // Env: OTEL_METRICS_ENDPOINT
  std::optional<std::string> endpoint{};

  // How often the PeriodicExportingMetricReader wakes up, collects all registered
  // instruments, and calls the exporter.
  // Env: OTEL_METRICS_READER_EXPORT_INTERVAL_MS  (default: 5000 ms)
  std::chrono::milliseconds reader_export_interval{ std::chrono::seconds(5) };

  // Maximum time the reader waits for a single export call to complete before
  // abandoning it and recording an error.
  // Env: OTEL_METRICS_READER_EXPORT_TIMEOUT_MS  (default: 500 ms)
  std::chrono::milliseconds reader_export_timeout{ std::chrono::milliseconds(500) };

  // Aggregation temporality forwarded to the OTLP exporter.
  // "cumulative" — counters and histograms always report totals since process start.
  //                Required by Prometheus, which expects monotonically increasing counters.
  // "delta"      — each batch contains only the delta since the previous export.
  //                Preferred by some managed backends (e.g. Lightstep, Dynatrace).
  // "low_memory" — delta for synchronous instruments, cumulative for asynchronous.
  // "unspecified" — let the exporter decide based on instrument type.
  // Env: OTEL_METRICS_EXPORTER_AGGREGATION_TEMPORALITY  (default: "cumulative")
  std::string exporter_aggregation_temporality{ "cumulative" };

  // HTTP request timeout for a single OTLP export POST.
  // Env: OTEL_METRICS_EXPORTER_TIMEOUT_MS  (default: 30000 ms)
  std::chrono::milliseconds exporter_timeout{ std::chrono::milliseconds(30000) };

  // Enable TLS for the OTLP HTTP connection.
  // Env: OTEL_METRICS_USE_SSL_CREDENTIALS  (truthy: yes / y / on / true / 1)
  bool use_ssl_credentials{ false };

  // Path to a CA certificate file used to verify the OTLP server's TLS certificate.
  // Env: OTEL_METRICS_SSL_CREDENTIALS_CACERT
  std::optional<std::string> ssl_credentials_cacert{};

  // Extra HTTP headers injected into every OTLP request, e.g. auth tokens for
  // managed observability backends (Grafana Cloud, Honeycomb, etc.).
  // Env: OTEL_METRICS_HEADERS  (format: "Key1=Value1,Key2=Value2")
  std::map<std::string, std::string> headers{};

  // Optional payload compression algorithm applied to the HTTP request body.
  // Env: OTEL_METRICS_COMPRESSION  (e.g. "gzip")
  std::optional<std::string> compression{};

  // Explicit bucket boundaries applied to every registered HISTOGRAM instrument
  // via a process-wide catch-all View (see apply_opentelemetry_meter_options).
  //
  // Why custom boundaries are necessary
  // ------------------------------------
  // The Couchbase C++ SDK records db.client.operation.duration in seconds,
  // converting internally from microsecond-resolution measurements before calling
  // into the OTel histogram.  The OpenTelemetry SDK's built-in default boundaries:
  //
  //   [0, 5, 10, 25, 50, 75, 100, 250, 500, 750, 1000, 2500, 5000, 7500, 10000] s
  //
  // are calibrated for millisecond values (a convention rooted in the HTTP latency
  // metrics of the OTel semantic conventions).  For second-valued Couchbase histograms
  // — where a well-connected operation typically completes in under 10 ms — almost
  // every sample would land in the first bucket, making p50/p99 estimates meaningless.
  //
  // The defaults below
  // -------------------
  // Eight boundaries spanning 100 µs to 10 s, chosen to match the Couchbase Java
  // SDK's canonical nanosecond recommendation, scaled to seconds (÷ 1 000 000 000):
  //
  //   Java SDK (nanoseconds)    C++ SDK (seconds)    Human-readable
  //              100 000   →        0.0001            100 µs
  //              250 000   →        0.00025           250 µs
  //              500 000   →        0.0005            500 µs
  //            1 000 000   →        0.001               1 ms
  //           10 000 000   →        0.01               10 ms
  //          100 000 000   →        0.1               100 ms
  //        1 000 000 000   →        1.0                 1 s
  //       10 000 000 000   →       10.0                10 s
  //
  // To use different boundaries, assign to this field before calling
  // apply_opentelemetry_meter_options().  No environment-variable override is
  // provided: safe textual round-trip encoding of a floating-point boundary list
  // is non-trivial and error-prone.
  std::vector<double> histogram_boundaries{
    0.0001,  // 100 µs
    0.00025, // 250 µs
    0.0005,  // 500 µs
    0.001,   //   1 ms
    0.01,    //  10 ms
    0.1,     // 100 ms
    1.0,     //   1 s
    10.0,    //  10 s
  };

  static void fill_from_env(opentelemetry_metrics_config& config);
};

// Runtime configuration for the OTLP traces pipeline.
// All fields can be overridden via environment variables; see fill_from_env().
struct opentelemetry_traces_config {
  // OTLP HTTP endpoint for traces.
  // Default when unset: http://localhost:4318/v1/traces (OTel Collector HTTP port).
  // Env: OTEL_TRACES_ENDPOINT
  std::optional<std::string> endpoint{};

  // HTTP request timeout for a single OTLP export POST.
  // Env: OTEL_TRACES_EXPORTER_TIMEOUT_MS  (default: 30000 ms)
  std::chrono::milliseconds exporter_timeout{ std::chrono::milliseconds(30000) };

  // Extra HTTP headers injected into every OTLP request.
  // Env: OTEL_TRACES_HEADERS  (format: "Key1=Value1,Key2=Value2")
  std::map<std::string, std::string> headers{};

  // Optional payload compression algorithm applied to the HTTP request body.
  // Env: OTEL_TRACES_COMPRESSION  (e.g. "gzip")
  std::optional<std::string> compression{};

  static void fill_from_env(opentelemetry_traces_config& config);
};

// Top-level runtime configuration for the demo program.
// Every field is populated from an environment variable via create_from_env();
// defaults are chosen to work against a stock local Couchbase cluster.
struct program_config {
  // Couchbase connection string.  Env: CONNECTION_STRING  (default: couchbase://127.0.0.1)
  std::string connection_string{ "couchbase://127.0.0.1" };
  // Couchbase RBAC username.  Env: USER_NAME  (default: Administrator)
  std::string user_name{ "Administrator" };
  // Couchbase RBAC password.  Env: PASSWORD  (default: password)
  std::string password{ "password" };
  // Bucket to write into.  Env: BUCKET_NAME  (default: default)
  std::string bucket_name{ "default" };
  // Scope within the bucket.  Env: SCOPE_NAME  (default: _default)
  std::string scope_name{ couchbase::scope::default_name };
  // Collection within the scope.  Env: COLLECTION_NAME  (default: _default)
  std::string collection_name{ couchbase::collection::default_name };
  // Optional SDK connection profile; e.g. "wan_development" for high-latency networks.
  // Env: PROFILE
  std::optional<std::string> profile{};
  // Print full Couchbase SDK trace-level logs to stderr.  Env: VERBOSE  (yes/y/on/true/1)
  bool verbose{ false };
  // Print OTel SDK internal diagnostics (export errors, sampler events) to stderr.
  // Env: OTEL_VERBOSE  (yes/y/on/true/1)
  bool otel_verbose{ false };
  // Number of iterations to run the main upsert/get loop.  Env: NUM_ITERATIONS  (default: 1000)
  std::size_t num_iterations{ 1000 };

  opentelemetry_metrics_config metrics_config{};
  opentelemetry_traces_config traces_config{};

  static auto create_from_env() -> program_config;
  void dump();
};

namespace
{
// service.name and service.version are attached to every exported span and metric
// data point as OTel Resource attributes.  In Jaeger the service name appears in
// the "Service" drop-down; in Prometheus it becomes part of the job/instance labels.
constexpr auto* k_service_name{ "inventory-service" };
constexpr auto* k_service_version{ "1.0.0" };

// An OTel Resource describes the entity producing telemetry — in this case, this
// process.  Attributes set here are stamped on every exported span and metric batch.
// At minimum, service.name must be set; it is the primary key for all trace and
// metric queries in Jaeger, Prometheus, and Grafana.
auto
make_otel_resource() -> opentelemetry::sdk::resource::Resource
{
  return opentelemetry::sdk::resource::Resource::Create({
    { "service.name", k_service_name },
    { "service.version", k_service_version },
  });
}
} // namespace

// Sets up the OpenTelemetry metrics pipeline and wires it into the Couchbase cluster
// options so the SDK records operation latency, retry counts, and timeout events.
//
// Pipeline:
//   Couchbase SDK instruments
//     → couchbase::metrics::otel_meter  (SDK adapter, implements couchbase meter interface)
//     → global OTel MeterProvider
//     → ViewRegistry                    (custom histogram buckets: 100 µs … 10 s)
//     → PeriodicExportingMetricReader   (fires every reader_export_interval, default 5 s)
//     → OtlpHttpMetricExporter          (HTTP POST to OTLP endpoint as JSON)
//     → OTel Collector                  (receives on :4318, exposes Prometheus scrape on :8889)
//     → Prometheus                      (scrapes :8889 every 15 s)
//
// Returns the SDK-level MeterProvider so the caller can call ForceFlush/Shutdown before
// exit and guarantee all buffered metric data points are flushed to the collector.
// tag::metrics-otel-prometheus[]
auto
apply_opentelemetry_meter_options(couchbase::cluster_options& options,
                                  const opentelemetry_metrics_config& metrics)
  -> std::shared_ptr<opentelemetry::sdk::metrics::MeterProvider>
{
  // Resource is stamped on every exported metric batch so Prometheus can identify
  // which process produced the data.
  auto resource = make_otel_resource();

  // --- OTLP HTTP metric exporter ---
  // Serialises each metric batch as a JSON protobuf payload and POSTs it to the
  // OTLP HTTP endpoint.  The OTel Collector receives batches on :4318, translates
  // them to Prometheus format, and exposes a scrape endpoint on :8889.
  opentelemetry::exporter::otlp::OtlpHttpMetricExporterOptions exporter_options{};
  if (auto endpoint = metrics.endpoint; endpoint) {
    // Override the built-in default (http://localhost:4318/v1/metrics) with the
    // value from OTEL_METRICS_ENDPOINT.
    exporter_options.url = endpoint.value();
  }
  exporter_options.content_type = opentelemetry::exporter::otlp::HttpRequestContentType::kJson;

  // Temporality controls how the SDK accumulates values between export intervals:
  //   kCumulative — each batch reports totals from process start (required for Prometheus
  //                 because it models counters as monotonically increasing sequences).
  //   kDelta      — each batch reports only the delta since the previous export; saves
  //                 memory and works well with backends that handle delta natively.
  //   kLowMemory  — delta for synchronous instruments, cumulative for asynchronous.
  if (auto temporality = metrics.exporter_aggregation_temporality;
      temporality.empty() || temporality == "unspecified") {
    exporter_options.aggregation_temporality =
      opentelemetry::exporter::otlp::PreferredAggregationTemporality::kUnspecified;
  } else if (temporality == "delta") {
    exporter_options.aggregation_temporality =
      opentelemetry::exporter::otlp::PreferredAggregationTemporality::kDelta;
  } else if (temporality == "cumulative") {
    exporter_options.aggregation_temporality =
      opentelemetry::exporter::otlp::PreferredAggregationTemporality::kCumulative;
  } else if (temporality == "low_memory") {
    exporter_options.aggregation_temporality =
      opentelemetry::exporter::otlp::PreferredAggregationTemporality::kLowMemory;
  }

  exporter_options.timeout = metrics.exporter_timeout;
  if (!metrics.headers.empty()) {
    for (const auto& [key, value] : metrics.headers) {
      exporter_options.http_headers.insert({ key, value });
    }
  }
  if (auto compression = metrics.compression; compression) {
    exporter_options.compression = compression.value();
  }

  auto exporter =
    opentelemetry::exporter::otlp::OtlpHttpMetricExporterFactory::Create(exporter_options);

  // --- Periodic metric reader ---
  // Wakes up on a background thread every reader_export_interval (default 5 s),
  // calls Collect() on every registered meter to gather current instrument values,
  // then passes the batch to the exporter.  reader_export_timeout caps how long
  // a single export HTTP call may run before it is abandoned.
  opentelemetry::sdk::metrics::PeriodicExportingMetricReaderOptions reader_options{};
  reader_options.export_interval_millis = metrics.reader_export_interval;
  reader_options.export_timeout_millis = metrics.reader_export_timeout;

  std::unique_ptr<opentelemetry::sdk::metrics::MetricReader> reader{
    new opentelemetry::sdk::metrics::PeriodicExportingMetricReader(std::move(exporter),
                                                                   reader_options)
  };

  // --- Histogram View: explicit bucket boundaries ---
  // Registers a single catch-all View that overrides the aggregation for every
  // HISTOGRAM instrument in the process, replacing the OTel SDK's built-in
  // default boundaries with the calibrated set from metrics.histogram_boundaries.
  //
  // The InstrumentSelector matches any instrument name and any unit (both wildcarded),
  // filtered to HISTOGRAM type only.  The MeterSelector is similarly open, so the
  // View applies regardless of which library or SDK component registered the meter.
  // The View itself carries no name or description, so each instrument retains its
  // own identity; only the aggregation configuration is overridden.
  //
  // This View must be registered before the first Meter is obtained from the
  // provider — AddView is not thread-safe and has no effect on instruments that
  // are already recording.  Wiring it into the ViewRegistry at MeterContext
  // construction time (before SetMeterProvider is called) satisfies that constraint.
  auto histogram_config =
    std::make_shared<opentelemetry::sdk::metrics::HistogramAggregationConfig>();
  histogram_config->boundaries_ = metrics.histogram_boundaries;

  auto view_registry = opentelemetry::sdk::metrics::ViewRegistryFactory::Create();
  view_registry->AddView(
    // Select all HISTOGRAM instruments, irrespective of name or unit.
    // InstrumentSelector uses kPattern for name ("*" = wildcard) but kExact for
    // unit ("" = MatchEverythingPattern = match-all; "*" would be a literal match).
    opentelemetry::sdk::metrics::InstrumentSelectorFactory::Create(
      opentelemetry::sdk::metrics::InstrumentType::kHistogram, "*", ""),
    // Apply to every meter, regardless of name, version, or schema URL.
    // MeterSelector uses kExact matching for all three fields; "" produces
    // MatchEverythingPattern (match-all). "*" would be a literal string match.
    opentelemetry::sdk::metrics::MeterSelectorFactory::Create("", "", ""),
    // Inherit the instrument's name and description; only the aggregation changes.
    opentelemetry::sdk::metrics::ViewFactory::Create(
      "", "", opentelemetry::sdk::metrics::AggregationType::kHistogram, histogram_config));

  // --- MeterProvider assembly ---
  // MeterContext owns the ViewRegistry (which controls aggregation and filtering rules
  // per instrument) and the list of active readers.  The MeterProvider is the factory
  // that creates Meter objects; both application code and the Couchbase SDK adapter
  // call GetMeter() on it to obtain a scoped meter.
  auto context =
    opentelemetry::sdk::metrics::MeterContextFactory::Create(std::move(view_registry), resource);
  context->AddMetricReader(std::move(reader));

  // Promote to shared_ptr (MeterProviderFactory returns unique_ptr) so that
  // ForceFlush/Shutdown can be called from main() after the cluster is closed.
  auto sdk_provider = std::shared_ptr<opentelemetry::sdk::metrics::MeterProvider>(
    opentelemetry::sdk::metrics::MeterProviderFactory::Create(std::move(context)).release());

  // Register as the process-wide global provider so that any call to
  // opentelemetry::metrics::Provider::GetMeterProvider() returns this instance.
  opentelemetry::metrics::Provider::SetMeterProvider(sdk_provider);

  // --- Couchbase SDK integration ---
  // options.metrics().enable(true) activates the SDK's internal metric collection.
  // otel_meter is the bridge adapter: it implements the couchbase::metrics::meter
  // interface and forwards every record() call to the underlying OTel Meter.
  // The SDK uses it to record per-operation latency histograms, retry counters,
  // and timeout events — all labelled with bucket, scope, collection, and operation type.
  options.metrics().enable(true);
  options.metrics().meter(std::make_shared<couchbase::metrics::otel_meter>(
    opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter(k_service_name,
                                                                   k_service_version)));

  return sdk_provider;
}
// end::metrics-otel-prometheus[]

// Sets up the OpenTelemetry tracing pipeline and wires it into the Couchbase cluster
// options so every SDK operation emits child spans under the caller-supplied parent.
//
// Pipeline:
//   Application span  (created in main with tracer->StartSpan)
//     → Couchbase SDK child spans  (otel_request_tracer adapter)
//     → BatchSpanProcessor         (buffers completed spans in memory)
//     → OtlpHttpExporter           (HTTP POST to OTLP endpoint as JSON)
//     → OTel Collector             (receives on :4318, forwards to Jaeger via OTLP/gRPC)
//     → Jaeger                     (stores and visualises traces)
//
// Returns the SDK-level TracerProvider so the caller can call ForceFlush/Shutdown before
// exit and guarantee all buffered spans are exported to the collector.
auto
apply_opentelemetry_tracer_options(couchbase::cluster_options& options,
                                   const opentelemetry_traces_config& traces)
  -> std::shared_ptr<opentelemetry::sdk::trace::TracerProvider>
{
  // Resource is stamped on every exported span so Jaeger can group them under the
  // correct service name in the UI.
  auto resource = make_otel_resource();

  // --- OTLP HTTP span exporter ---
  // Serialises completed spans as JSON protobuf and POSTs them to the OTLP HTTP
  // endpoint.  The OTel Collector forwards them to Jaeger via OTLP/gRPC.
  opentelemetry::exporter::otlp::OtlpHttpExporterOptions exporter_options{};
  // Default: http://localhost:4318/v1/traces; override with OTEL_TRACES_ENDPOINT.
  if (auto endpoint = traces.endpoint; endpoint) {
    exporter_options.url = endpoint.value();
  }
  exporter_options.content_type = opentelemetry::exporter::otlp::HttpRequestContentType::kJson;
  exporter_options.timeout = traces.exporter_timeout;
  if (!traces.headers.empty()) {
    for (const auto& [key, value] : traces.headers) {
      exporter_options.http_headers.insert({ key, value });
    }
  }
  if (auto compression = traces.compression; compression) {
    exporter_options.compression = compression.value();
  }

  auto exporter = opentelemetry::exporter::otlp::OtlpHttpExporterFactory::Create(exporter_options);

  // --- Batch span processor ---
  // Accumulates completed spans in an in-memory ring buffer and exports them in
  // batches to reduce HTTP overhead.  Default tuning:
  //   max_queue_size        = 2048 spans
  //   max_export_batch_size = 512 spans
  //   schedule_delay        = 5 s  (background flush interval)
  // ForceFlush() (called in main before exit) drains the buffer synchronously,
  // ensuring no spans are lost even when the program runs for less than 5 s.
  opentelemetry::sdk::trace::BatchSpanProcessorOptions processor_options{};
  auto processor = opentelemetry::sdk::trace::BatchSpanProcessorFactory::Create(std::move(exporter),
                                                                                processor_options);

  // --- Sampler ---
  // AlwaysOnSampler records and exports every span (100 % sampling rate).
  // This is appropriate for development and short-lived demos.
  // For production services consider:
  //   ParentBased(TraceIdRatioBased(0.01))  — 1 % head-based sampling
  // which dramatically reduces export volume while preserving full traces
  // for a statistically representative fraction of requests.
  auto sampler = opentelemetry::sdk::trace::AlwaysOnSamplerFactory::Create();

  // --- TracerProvider assembly ---
  // Ties together the processor (which holds the exporter), the resource, and the
  // sampler into a single provider.  Promote to shared_ptr so ForceFlush/Shutdown
  // are accessible from main() after the Couchbase cluster is closed.
  auto sdk_provider = std::shared_ptr<opentelemetry::sdk::trace::TracerProvider>(
    opentelemetry::sdk::trace::TracerProviderFactory::Create(
      std::move(processor), resource, std::move(sampler))
      .release());

  // Register as the process-wide global provider so GetTracerProvider() works anywhere.
  opentelemetry::trace::Provider::SetTracerProvider(sdk_provider);

  // --- Couchbase SDK integration ---
  // otel_request_tracer is the bridge adapter: it implements the
  // couchbase::tracing::request_tracer interface and forwards every
  // start_span() / end_span() call to the underlying OTel Tracer.
  // When an operation's options carry a parent_span (set in main via
  // otel_request_span), the SDK creates its internal spans (upsert,
  // dispatch_to_server, etc.) as children of that parent, producing a
  // complete nested trace hierarchy in Jaeger.
  options.tracing().tracer(std::make_shared<couchbase::tracing::otel_request_tracer>(
    opentelemetry::trace::Provider::GetTracerProvider()->GetTracer(k_service_name,
                                                                   k_service_version)));

  return sdk_provider;
}

int
main()
{
  // Capture the program start time so we can report the total run duration as a
  // diagnostic metric.  This is intentionally the very first statement so the
  // measurement includes connection setup, cluster operations, and teardown.
  const auto demo_start = std::chrono::steady_clock::now();

  auto config = program_config::create_from_env();
  config.dump(); // Print all resolved configuration values to stdout before doing anything.

  // Optional: enable full Couchbase SDK trace-level logging to stderr.
  // Very verbose — useful when debugging connection or protocol issues.
  if (config.verbose) {
    couchbase::logger::initialize_console_logger();
    couchbase::logger::set_level(couchbase::logger::log_level::trace);
  }

  // Optional: install the diagnostic log handler so OTel SDK internal messages
  // (failed export attempts, sampler decisions, etc.) appear on stderr instead of
  // being silently discarded.  Useful when the collector is unreachable.
  if (config.otel_verbose) {
    opentelemetry::sdk::common::internal_log::GlobalLogHandler::SetLogHandler(
      std::make_shared<otel_diagnostic_log_handler>());
  }

  auto options = couchbase::cluster_options(config.user_name, config.password);
  if (config.profile) {
    options.apply_profile(config.profile.value());
  }

  // OTel metrics and tracing MUST be wired into cluster options before calling
  // couchbase::cluster::connect().  The SDK reads the meter and tracer from the
  // options object at connect time; changing them afterwards has no effect.
  auto meter_provider = apply_opentelemetry_meter_options(options, config.metrics_config);
  auto tracer_provider = apply_opentelemetry_tracer_options(options, config.traces_config);

  auto [connect_err, cluster] =
    couchbase::cluster::connect(config.connection_string, options).get();
  if (connect_err) {
    std::cout << "Unable to connect to the cluster. ec: " << connect_err.message() << "\n";
  } else {
    auto collection = cluster.bucket(config.bucket_name)
                        .scope(config.scope_name)
                        .collection(config.collection_name);

    // --- Per-iteration diagnostic metric ---
    // Instruments must be created once and reused across recordings; creating a
    // new histogram on every iteration is wasteful and produces duplicate
    // instrument warnings from the OTel SDK.
    // In Prometheus:
    //   inventory_demo_iteration_duration_ms_bucket{...}
    //   inventory_demo_iteration_duration_ms_sum
    //   inventory_demo_iteration_duration_ms_count
    auto app_meter = opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter(
      k_service_name, k_service_version);
    auto iteration_duration =
      app_meter->CreateDoubleHistogram("inventory_demo_iteration_duration",
                                       "Wall-clock duration of a single upsert+get iteration",
                                       "ms");

    // --- Application-level root span ---
    // All Couchbase SDK operations in this block are given cb_parent as their
    // parent_span so the SDK's internal spans (upsert, get,
    // dispatch_to_server, …) appear as children of "update-inventory" in Jaeger,
    // giving a single trace that covers one loop iteration.
    auto tracer = opentelemetry::trace::Provider::GetTracerProvider()->GetTracer(k_service_name,
                                                                                 k_service_version);
    std::size_t error_count{ 0 };
    std::string last_error{};
    const auto print_progress = [&](std::size_t iteration) {
      const std::size_t done = iteration + 1;
      const int pct = static_cast<int>(done * 100 / config.num_iterations);
      constexpr int bar_width = 30;
      const int filled = pct * bar_width / 100;
      std::string bar(static_cast<std::size_t>(filled), '=');
      if (filled < bar_width) {
        bar += '>';
        bar += std::string(static_cast<std::size_t>(bar_width - filled - 1), ' ');
      }
      std::cout << "\r[" << bar << "] " << std::setw(3) << pct << "% " << done << "/"
                << config.num_iterations << "  errors: " << error_count;
      if (!last_error.empty()) {
        std::cout << "  last error: " << last_error;
      }
      std::cout << "   " << std::flush;
    };

    for (std::size_t iteration = 0; iteration < config.num_iterations; ++iteration) {
      const std::string document_id{ "item::WIDGET-" + std::to_string(iteration) };

      const auto iter_start = std::chrono::steady_clock::now();

      auto top_span = tracer->StartSpan("update-inventory");

      // WithActiveSpan sets top_span as the active span on the current thread's context.
      // Any OTel-instrumented library called from this scope that does automatic
      // context propagation will automatically use top_span as its parent.
      auto scope = tracer->WithActiveSpan(top_span);

      // otel_request_span bridges the OTel Span type to the
      // couchbase::tracing::request_span interface expected by the SDK's parent_span option.
      auto cb_parent = std::make_shared<couchbase::tracing::otel_request_span>(top_span);

      {
        const tao::json::value item{
          { "name", "Widget Pro" }, { "sku", "WIDGET-001" }, { "category", "widgets" },
          { "quantity", 42 },       { "price", 29.99 },
        };

        // parent_span(cb_parent) attaches this operation to the "update-inventory" trace.
        // The SDK emits an "upsert" child span with "request_encoding" and
        // "dispatch_to_server" grandchildren capturing serialization and the
        // server round-trip duration.
        auto [err, resp] =
          collection.upsert(document_id, item, couchbase::upsert_options{}.parent_span(cb_parent))
            .get();
        if (err.ec()) {
          ++error_count;
          last_error = "upsert: " + err.message();
        }
      }
      {
        // Same parent span as the upsert: both operations appear under the same root
        // trace in Jaeger, making it easy to see the full sequence at a glance.
        // The SDK emits a "get" child span with a "dispatch_to_server" grandchild.
        auto [err, resp] =
          collection.get(document_id, couchbase::get_options{}.parent_span(cb_parent)).get();
        if (err.ec()) {
          ++error_count;
          last_error = "get: " + err.message();
        }
      }

      print_progress(iteration);

      // Mark the root span successful and close it.  The SDK child spans (upsert,
      // get) are already ended by the time collection.upsert/get return.
      top_span->SetStatus(opentelemetry::trace::StatusCode::kOk);
      top_span->End();

      const auto iter_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - iter_start)
                                     .count();
      iteration_duration->Record(iter_elapsed_ms, opentelemetry::context::Context{});
    }
    std::cout << "\n";
  }

  cluster.close().get();

  // --- Demo-app diagnostic metric: total run duration ---
  //
  // Record the total wall-clock time from process start to cluster close as a
  // single histogram sample.  This serves as a simple end-to-end smoke-test for
  // the metrics pipeline: if you can see this metric in Prometheus it means the
  // full chain (OTel SDK → OTLP exporter → OTel Collector → Prometheus scrape)
  // is working correctly.
  //
  // How to find it in Prometheus (http://localhost:9090):
  //   inventory_demo_run_duration_ms_bucket
  //   inventory_demo_run_duration_ms_sum
  //   inventory_demo_run_duration_ms_count
  //
  // The metric carries the service.name="inventory-service" resource attribute so
  // you can also filter by {job="inventory-service"} or similar in Grafana.
  {
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - demo_start)
                              .count();
    std::cout << "Demo run duration: " << elapsed_ms << " ms\n";

    auto app_meter = opentelemetry::metrics::Provider::GetMeterProvider()->GetMeter(
      k_service_name, k_service_version);
    auto run_duration =
      app_meter->CreateDoubleHistogram("inventory_demo_run_duration",
                                       "Total wall-clock duration of the inventory demo run, "
                                       "from process start to cluster close",
                                       "ms");
    run_duration->Record(elapsed_ms, opentelemetry::context::Context{});
  }

  // --- Flush before exit and let providers shut down via their destructors ---
  //
  // ForceFlush ensures all buffered data is exported before the process exits.
  // For metrics this is critical: PeriodicExportingMetricReader::OnShutDown()
  // does not do a final collection pass, so any data accumulated since the last
  // export interval would be silently dropped without it.  For traces,
  // BatchSpanProcessor::Shutdown() does drain the queue, so ForceFlush is
  // redundant there — but kept for symmetry and safety.
  //
  // Shutdown() itself is intentionally not called here.  Both destructors call
  // Shutdown() on their underlying context unconditionally, so an explicit call
  // here would trigger a double-invocation and the warning:
  //   "[MeterContext::Shutdown] Shutdown can be invoked only once."
  // The global providers set via SetMeterProvider/SetTracerProvider hold a
  // reference; those globals are released at static-destruction time, the
  // refcount drops to zero, and Shutdown() is invoked exactly once via the
  // destructor.
  tracer_provider->ForceFlush();
  meter_provider->ForceFlush();

  return 0;
}

void
opentelemetry_traces_config::fill_from_env(opentelemetry_traces_config& config)
{
  if (const auto* val = getenv("OTEL_TRACES_ENDPOINT"); val != nullptr) {
    config.endpoint = val;
  }
  if (const auto* val = getenv("OTEL_TRACES_EXPORTER_TIMEOUT_MS"); val != nullptr) {
    config.exporter_timeout = std::chrono::milliseconds(std::stoul(val));
  }
  if (const auto* val = getenv("OTEL_TRACES_HEADERS"); val != nullptr) {
    // comma-separated "key=value" pairs, e.g. "Authorization=Bearer token,x-tenant=foo"
    std::istringstream stream(val);
    std::string pair;
    while (std::getline(stream, pair, ',')) {
      if (const auto eq = pair.find('='); eq != std::string::npos) {
        config.headers[pair.substr(0, eq)] = pair.substr(eq + 1);
      }
    }
  }
  if (const auto* val = getenv("OTEL_TRACES_COMPRESSION"); val != nullptr) {
    config.compression = val;
  }
}

void
opentelemetry_metrics_config::fill_from_env(opentelemetry_metrics_config& config)
{
  if (const auto* val = getenv("OTEL_METRICS_ENDPOINT"); val != nullptr) {
    config.endpoint = val;
  }
  if (const auto* val = getenv("OTEL_METRICS_READER_EXPORT_INTERVAL_MS"); val != nullptr) {
    config.reader_export_interval = std::chrono::milliseconds(std::stoul(val));
  }
  if (const auto* val = getenv("OTEL_METRICS_READER_EXPORT_TIMEOUT_MS"); val != nullptr) {
    config.reader_export_timeout = std::chrono::milliseconds(std::stoul(val));
  }
  if (const auto* val = getenv("OTEL_METRICS_EXPORTER_AGGREGATION_TEMPORALITY"); val != nullptr) {
    config.exporter_aggregation_temporality = val;
  }
  if (const auto* val = getenv("OTEL_METRICS_EXPORTER_TIMEOUT_MS"); val != nullptr) {
    config.exporter_timeout = std::chrono::milliseconds(std::stoul(val));
  }
  if (const auto* val = getenv("OTEL_METRICS_USE_SSL_CREDENTIALS"); val != nullptr) {
    for (const auto& truth : truthy_values) {
      if (val == truth) {
        config.use_ssl_credentials = true;
        break;
      }
    }
  }
  if (const auto* val = getenv("OTEL_METRICS_SSL_CREDENTIALS_CACERT"); val != nullptr) {
    config.ssl_credentials_cacert = val;
  }
  if (const auto* val = getenv("OTEL_METRICS_HEADERS"); val != nullptr) {
    // comma-separated "key=value" pairs, e.g. "Authorization=Bearer token,x-tenant=foo"
    std::istringstream stream(val);
    std::string pair;
    while (std::getline(stream, pair, ',')) {
      if (const auto eq = pair.find('='); eq != std::string::npos) {
        config.headers[pair.substr(0, eq)] = pair.substr(eq + 1);
      }
    }
  }
  if (const auto* val = getenv("OTEL_METRICS_COMPRESSION"); val != nullptr) {
    config.compression = val;
  }
}

auto
program_config::create_from_env() -> program_config
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
    config.profile = val;
  }
  if (const auto* val = getenv("VERBOSE"); val != nullptr) {
    for (const auto& truth : truthy_values) {
      if (val == truth) {
        config.verbose = true;
        break;
      }
    }
  }
  if (const auto* val = getenv("OTEL_VERBOSE"); val != nullptr) {
    for (const auto& truth : truthy_values) {
      if (val == truth) {
        config.otel_verbose = true;
        break;
      }
    }
  }
  if (const auto* val = getenv("NUM_ITERATIONS"); val != nullptr) {
    config.num_iterations = std::stoul(val);
  }

  opentelemetry_metrics_config::fill_from_env(config.metrics_config);
  opentelemetry_traces_config::fill_from_env(config.traces_config);

  return config;
}

void
program_config::dump()
{
  std::cout << "CONNECTION_STRING: " << quote(connection_string) << "\n";
  std::cout << "        USER_NAME: " << quote(user_name) << "\n";
  std::cout << "         PASSWORD: [HIDDEN]\n";
  std::cout << "      BUCKET_NAME: " << quote(bucket_name) << "\n";
  std::cout << "       SCOPE_NAME: " << quote(scope_name) << "\n";
  std::cout << "  COLLECTION_NAME: " << quote(collection_name) << "\n";
  std::cout << "          VERBOSE: " << std::boolalpha << verbose << "\n";
  std::cout << "     OTEL_VERBOSE: " << std::boolalpha << otel_verbose << "\n";
  std::cout << "   NUM_ITERATIONS: " << num_iterations << "\n";
  std::cout << "          PROFILE: " << quote(profile) << "\n";
  std::cout << "\n";

  // clang-format off
  std::cout << "                        OTEL_METRICS_ENDPOINT: " << quote(metrics_config.endpoint) << "\n";
  std::cout << "       OTEL_METRICS_READER_EXPORT_INTERVAL_MS: " << metrics_config.reader_export_interval.count() << "\n";
  std::cout << "        OTEL_METRICS_READER_EXPORT_TIMEOUT_MS: " << metrics_config.reader_export_timeout.count() << "\n";
  std::cout << "OTEL_METRICS_EXPORTER_AGGREGATION_TEMPORALITY: " << quote(metrics_config.exporter_aggregation_temporality) << "\n";
  std::cout << "             OTEL_METRICS_EXPORTER_TIMEOUT_MS: " << metrics_config.exporter_timeout.count() << "\n";
  std::cout << "             OTEL_METRICS_USE_SSL_CREDENTIALS: " << std::boolalpha << metrics_config.use_ssl_credentials << "\n";
  std::cout << "          OTEL_METRICS_SSL_CREDENTIALS_CACERT: " << quote(metrics_config.ssl_credentials_cacert)<< "\n";
  std::cout << "                     OTEL_METRICS_COMPRESSION: " << quote(metrics_config.compression)<< "\n";
  {
    std::cout << "            OTEL_METRICS_HISTOGRAM_BOUNDARIES: [";
    bool first = true;
    for (const auto& b : metrics_config.histogram_boundaries) {
      if (!first) {
        std::cout << ", ";
      }
      std::cout << b;
      first = false;
    }
    std::cout << "]\n";
  }
  if (metrics_config.headers.empty()) {
    std::cout << "                         OTEL_METRICS_HEADERS: [NONE]\n";
  } else {
    bool first = true;
    for (const auto& [key, val] : metrics_config.headers) {
      std::cout << (first ?
               "                         OTEL_METRICS_HEADERS: " :
               "                                               ") << key << "="
                << val << "\n";
      first = false;
    }
  }
  std::cout << "\n";
  std::cout << "           OTEL_TRACES_ENDPOINT: " << quote(traces_config.endpoint) << "\n";
  std::cout << "OTEL_TRACES_EXPORTER_TIMEOUT_MS: " << traces_config.exporter_timeout.count() << "\n";
  std::cout << "        OTEL_TRACES_COMPRESSION: " << quote(traces_config.compression) << "\n";
  if (traces_config.headers.empty()) {
    std::cout << "            OTEL_TRACES_HEADERS: [NONE]\n";
  } else {
    bool first = true;
    for (const auto& [key, val] : traces_config.headers) {
      std::cout << (first ?
               "            OTEL_TRACES_HEADERS: " :
               "                                 ") << key << "="
                << val << "\n";
      first = false;
    }
  }
  // clang-format on
  std::cout << "\n";
}
