# Telemetry Cluster

A local observability stack used by the `inventory_with_opentelemetry` example to
receive, store, and visualise the metrics and traces that the Couchbase C++ SDK
emits via OpenTelemetry.

> **Supported signals**
> The Couchbase C++ SDK currently supports **metrics** and **traces** only.
> It does not emit logs via OpenTelemetry.  Loki and Promtail are included in
> this stack for completeness but receive no data from the C++ SDK examples.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Your application  (inventory_with_opentelemetry)               │
│                                                                 │
│  OTLP/HTTP  http://localhost:4318/v1/traces    (traces)         │
│  OTLP/HTTP  http://localhost:4318/v1/metrics   (metrics)        │
└──────────────────────────┬──────────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────────┐
│  OpenTelemetry Collector                                         │
│  Listens: :4317 (OTLP gRPC)  :4318 (OTLP HTTP)                   │
│                                                                  │
│  traces  pipeline  ──► OTLP/gRPC ──► Jaeger       :4317 internal │
│  metrics pipeline  ──► Prometheus exporter        :8889          │
└──────────────────────────────────────────────────────────────────┘
          │                              │
          ▼                              ▼
  ┌───────────────┐             ┌────────────────┐
  │    Jaeger     │             │   Prometheus   │
  │  UI :16686    │             │   UI :9090     │
  └───────┬───────┘             └───────┬────────┘
          │                             │
          └──────────────┬──────────────┘
                         ▼
                  ┌─────────────┐
                  │   Grafana   │
                  │  UI :3000   │
                  └─────────────┘
```

Loki (`:3100`) and Promtail are running but are not part of the active signal
flow for C++ SDK examples.

---

## Services

### OpenTelemetry Collector

**Image:** `otel/opentelemetry-collector-contrib:0.99.0`
**Ports:** `4317` (OTLP gRPC), `4318` (OTLP HTTP)
**Config:** [`otel-collector-config.yaml`](otel-collector-config.yaml)

The Collector is the central hub of the stack.  The application sends all
telemetry to the Collector rather than directly to each backend, which:

- decouples the application from backend-specific protocols and endpoints,
- allows backend changes without recompiling the application,
- provides a single point for batching, retry, and filtering.

#### Pipeline configuration

```yaml
# otel-collector-config.yaml

receivers:
  otlp:
    protocols:
      grpc:   # listens on :4317
      http:   # listens on :4318  ← used by the C++ SDK example (OTLP/HTTP JSON)

exporters:
  otlp/jaeger:
    endpoint: jaeger:4317   # forward traces to Jaeger via OTLP/gRPC
    tls:
      insecure: true         # no TLS within the Docker network

  prometheus:
    endpoint: "0.0.0.0:8889" # expose a Prometheus scrape endpoint

  loki:
    endpoint: http://loki:3100/loki/api/v1/push  # unused by C++ SDK

service:
  pipelines:
    traces:
      receivers: [otlp]
      exporters: [otlp/jaeger]   # traces → Jaeger

    metrics:
      receivers: [otlp]
      exporters: [prometheus]    # metrics → Prometheus scrape endpoint

    logs:
      receivers: [otlp]
      exporters: [loki]          # logs → Loki (unused by C++ SDK)
```

The C++ SDK uses the **OTLP HTTP** receiver on `:4318`.  The gRPC receiver on
`:4317` is available but not used by the example.

---

### Jaeger

**Image:** `jaegertracing/all-in-one:1.62.0`
**Ports:** `16686` (UI), `14268` (HTTP collector — not used in this stack)
**UI:** http://localhost:16686

Jaeger is the distributed tracing backend.  It stores spans in memory (the
all-in-one image uses an in-process store; data is lost when the container
restarts) and provides a browser UI for searching and inspecting traces.

#### How the Collector delivers spans to Jaeger

The Collector's `otlp/jaeger` exporter sends spans to Jaeger's built-in OTLP
gRPC receiver (port `4317` on the `jaeger` container, within the Docker network).
The `14268` port (Thrift over HTTP) is exposed on the host but is not used by
this pipeline.

#### Finding traces from the example

1. Open http://localhost:16686 in a browser.
2. In the **Service** drop-down select `inventory-service`.
3. Click **Find Traces**.
4. Click on any result to open the trace.

The trace hierarchy produced by the example looks like:

```
update-inventory                     ← root span (application code in main())
  upsert                             ← Couchbase SDK upsert operation
    request_encoding                 ← document serialization
    dispatch_to_server               ← server round-trip latency
  get                                ← Couchbase SDK get operation
    dispatch_to_server               ← server round-trip latency
```

Operation spans (`upsert`, `get`) carry:

| Attribute                      | Example value         |
|--------------------------------|-----------------------|
| `db.system.name`               | `couchbase`           |
| `db.namespace`                 | `default`             |
| `db.operation.name`            | `upsert`              |
| `couchbase.collection.name`    | `_default`            |
| `couchbase.scope.name`         | `_default`            |
| `couchbase.service`            | `kv`                  |
| `couchbase.retries`            | `0`                   |

`dispatch_to_server` spans carry:

| Attribute                      | Example value         |
|--------------------------------|-----------------------|
| `network.peer.address`         | `127.0.0.1`           |
| `network.peer.port`            | `11210`               |
| `network.transport`            | `tcp`                 |
| `server.address`               | `127.0.0.1`           |
| `server.port`                  | `11210`               |
| `couchbase.operation_id`       | `0x1a2b`              |
| `couchbase.server_duration`    | `42`                  |
| `couchbase.local_id`           | `...`                 |

---

### Prometheus

**Image:** `prom/prometheus:v2.42.0`
**Port:** `9090`
**UI / API:** http://localhost:9090
**Config:** [`prometheus.yml`](prometheus.yml)

Prometheus scrapes metrics from the Collector's Prometheus exporter endpoint
every 15 seconds and stores them as time-series data.

#### Scrape configuration

```yaml
# prometheus.yml

global:
  scrape_interval: 15s   # default interval applied to all jobs

scrape_configs:
  - job_name: otel-collector
    static_configs:
      - targets: ['otel-collector:8889']   # Collector's Prometheus exporter
    scrape_interval: 15s
    metrics_path: /metrics

  # The following jobs scrape each service's own internal metrics
  # (not application metrics from the C++ SDK):
  - job_name: prometheus
    static_configs:
      - targets: ["prometheus:9090"]
  - job_name: jaeger
    static_configs:
      - targets: ["jaeger:16686"]
  - job_name: loki
    static_configs:
      - targets: ["loki:3100"]
```

The `otel-collector` job is the one that receives C++ SDK metrics.

#### Querying SDK metrics

Open the **Graph** tab at http://localhost:9090/graph and try these queries:

```promql
# All metric families emitted by the C++ SDK (search by prefix)
{__name__=~"db_couchbase.*"}

# Operation latency histogram — breakdown by operation type
histogram_quantile(0.99, rate(db_couchbase_operations_duration_bucket[1m]))

# Total completed operations
rate(db_couchbase_operations_total[1m])
```

> **Note:** Prometheus scrapes on a 15 s interval.  Run the example, wait up
> to 15 s, then query.  The PeriodicExportingMetricReader in the application
> must also have fired (default interval: 5 s) before any data appears.

---

### Grafana

**Image:** `grafana/grafana:9.3.2`
**Port:** `3000`
**UI:** http://localhost:3000
**Auth:** anonymous Admin (no login form — `GF_AUTH_ANONYMOUS_ENABLED=true`)

Grafana provides a unified browser UI that queries Prometheus (for metrics)
and Jaeger (for traces) through pre-provisioned data sources.

#### Pre-provisioned data sources

Defined in
[`grafana/provisioning/datasources/datasources.yml`](grafana/provisioning/datasources/datasources.yml):

| Name         | Type         | URL                       | Default |
|--------------|--------------|---------------------------|---------|
| `Prometheus` | `prometheus` | `http://prometheus:9090`  | yes     |
| `Jaeger`     | `jaeger`     | `http://jaeger:16686`     | no      |
| `Loki`       | `loki`       | `http://loki:3100`        | no      |

Because `GF_AUTH_DISABLE_LOGIN_FORM=true` is set, Grafana opens directly in
Admin mode — no username or password required.

#### Exploring data

- **Metrics:** Click **Explore** → select **Prometheus** → enter a PromQL
  expression such as `{__name__=~"db_couchbase.*"}`.
- **Traces:** Click **Explore** → select **Jaeger** → choose service
  `inventory-service` → click **Run query**.
- **Dashboards:** No dashboards are pre-provisioned.  Create one from
  **Dashboards → New → New dashboard** using the Prometheus and Jaeger
  data sources.

---

### Loki

**Image:** `grafana/loki:2.9.0`
**Port:** `3100`

Loki is a log aggregation backend.  It is running in this stack but **receives
no data from the Couchbase C++ SDK** because the SDK does not emit logs via
OpenTelemetry.  It is present so the stack can be extended for other languages
or for application-level structured logs that are forwarded via the OTel
Collector.

---

### Promtail

**Image:** `grafana/promtail:2.9.0`
**Config:** [`promtail-config.yml`](promtail-config.yml)

Promtail is the log-shipping agent that tails Docker container log files and
pushes them to Loki.  Its configuration is:

```yaml
clients:
  - url: http://loki:3100/loki/api/v1/push

scrape_configs:
  - job_name: docker
    static_configs:
      - labels:
          job: docker-logs
          __path__: /var/lib/docker/containers/*/*.log
```

This would ship the stdout/stderr of every Docker container to Loki.  In
practice it requires the Docker daemon's log driver to write JSON files to the
standard path (`/var/lib/docker/containers`), which is the default on Linux.

---

## Quick Start

### Prerequisites

- Docker Engine ≥ 20.10 with the Compose plugin (`docker compose` v2)
- A running Couchbase Server reachable at `couchbase://127.0.0.1`

### Start the stack

```bash
cd telemetry-cluster
docker compose up -d
```

All six containers start in dependency order.  Allow approximately 10 seconds
for every service to become healthy before sending telemetry.

### Verify the stack is healthy

```bash
docker compose ps
```

Expected output (all containers `Up`):

```
NAME            IMAGE                                        STATUS
grafana         grafana/grafana:9.3.2                        Up
jaeger          jaegertracing/all-in-one:1.62.0              Up
loki            grafana/loki:2.9.0                           Up
otel-collector  otel/opentelemetry-collector-contrib:0.99.0  Up
prometheus      prom/prometheus:v2.42.0                      Up
promtail        grafana/promtail:2.9.0                       Up
```

You can also verify each service responds:

```bash
# OTel Collector — health check
curl -s http://localhost:4318/

# Jaeger UI — check it loads
curl -s -o /dev/null -w "%{http_code}" http://localhost:16686/

# Prometheus — instant query
curl -s 'http://localhost:9090/api/v1/query?query=up' | python3 -m json.tool

# Grafana — health endpoint
curl -s http://localhost:3000/api/health | python3 -m json.tool
```

### Stop the stack

```bash
docker compose down
```

Data stored in Jaeger, Prometheus, and Grafana is **not** persisted to disk
(Grafana uses a named Docker volume `grafana-storage` that survives `down` but
is removed by `down -v`).  Jaeger and Prometheus use in-container memory only.

### Reset (remove all data)

```bash
docker compose down -v   # removes grafana-storage volume as well
```

---

## Sending telemetry from the example

Build and run the example from the repository root:

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target inventory_with_opentelemetry

# Run (adjust credentials and connection string as needed)
CONNECTION_STRING=couchbase://127.0.0.1 \
USER_NAME=Administrator \
PASSWORD=password \
BUCKET_NAME=default \
  ./build/examples/inventory_with_opentelemetry
```

The application defaults to `http://localhost:4318/v1/{traces,metrics}` which
points directly at the OTel Collector started above.

### Environment variable reference

#### Couchbase connection

| Variable            | Default                  | Description                                      |
|---------------------|--------------------------|--------------------------------------------------|
| `CONNECTION_STRING` | `couchbase://127.0.0.1`  | Couchbase connection string                      |
| `USER_NAME`         | `Administrator`          | RBAC username                                    |
| `PASSWORD`          | `password`               | RBAC password                                    |
| `BUCKET_NAME`       | `default`                | Bucket to write into                             |
| `SCOPE_NAME`        | `_default`               | Scope within the bucket                          |
| `COLLECTION_NAME`   | `_default`               | Collection within the scope                      |
| `PROFILE`           | *(unset)*                | SDK connection profile, e.g. `wan_development`   |

#### Diagnostics

| Variable       | Default | Description                                              |
|----------------|---------|----------------------------------------------------------|
| `VERBOSE`      | `false` | Couchbase SDK trace-level logs to stderr                 |
| `OTEL_VERBOSE` | `false` | OTel SDK internal diagnostic messages to stderr          |

#### Traces

| Variable                          | Default                                    | Description                                    |
|-----------------------------------|--------------------------------------------|------------------------------------------------|
| `OTEL_TRACES_ENDPOINT`            | `http://localhost:4318/v1/traces`          | OTLP HTTP endpoint for traces                  |
| `OTEL_TRACES_EXPORTER_TIMEOUT_MS` | `30000`                                    | HTTP request timeout (ms)                      |
| `OTEL_TRACES_HEADERS`             | *(unset)*                                  | Extra headers: `Key1=Val1,Key2=Val2`           |
| `OTEL_TRACES_COMPRESSION`         | *(unset)*                                  | Compression algorithm, e.g. `gzip`             |

#### Metrics

| Variable                                      | Default                                    | Description                                                       |
|-----------------------------------------------|--------------------------------------------|-------------------------------------------------------------------|
| `OTEL_METRICS_ENDPOINT`                       | `http://localhost:4318/v1/metrics`         | OTLP HTTP endpoint for metrics                                    |
| `OTEL_METRICS_READER_EXPORT_INTERVAL_MS`      | `5000`                                     | How often the reader collects and exports (ms)                    |
| `OTEL_METRICS_READER_EXPORT_TIMEOUT_MS`       | `500`                                      | Max time for one export call (ms)                                 |
| `OTEL_METRICS_EXPORTER_AGGREGATION_TEMPORALITY` | `cumulative`                             | `cumulative` / `delta` / `low_memory` / `unspecified`             |
| `OTEL_METRICS_EXPORTER_TIMEOUT_MS`            | `30000`                                    | HTTP request timeout (ms)                                         |
| `OTEL_METRICS_USE_SSL_CREDENTIALS`            | `false`                                    | Enable TLS (`yes`/`y`/`on`/`true`/`1`)                            |
| `OTEL_METRICS_SSL_CREDENTIALS_CACERT`         | *(unset)*                                  | Path to CA certificate file for TLS verification                  |
| `OTEL_METRICS_HEADERS`                        | *(unset)*                                  | Extra headers: `Key1=Val1,Key2=Val2`                              |
| `OTEL_METRICS_COMPRESSION`                    | *(unset)*                                  | Compression algorithm, e.g. `gzip`                                |

---

## Troubleshooting

### No traces appear in Jaeger

1. **Check the Collector is running:**
   ```bash
   docker compose ps otel-collector
   ```
2. **Check the Collector received the spans** (look for export log lines):
   ```bash
   docker compose logs otel-collector | grep -i trace
   ```
3. **Check the application exported before exiting.**
   The `BatchSpanProcessor` has a default `schedule_delay` of 5 s.  Without
   explicit `ForceFlush()`, a short-lived process exits before the first flush.
   The example calls `tracer_provider->ForceFlush()` before returning from
   `main()` to avoid this.
4. **Check `OTEL_VERBOSE=true`** output for HTTP errors from the exporter.

### No metrics appear in Prometheus

1. **Wait for the scrape interval.**  Prometheus scrapes the Collector every
   15 s.  The application's `PeriodicExportingMetricReader` fires every 5 s
   (default).  After `ForceFlush()` in the application, allow up to 15 s for
   Prometheus to scrape.
2. **Verify the Collector exposes the scrape endpoint:**
   ```bash
   curl -s http://localhost:8889/metrics | head -40
   ```
   You should see lines beginning with `# HELP` and `# TYPE` for
   `db_couchbase_*` metrics.
3. **Verify Prometheus is scraping the Collector:**
   Open http://localhost:9090/targets and confirm the `otel-collector` target
   is in `UP` state.

### Grafana shows "No data"

- For Prometheus panels, confirm the metric exists:
  http://localhost:9090/graph — enter `{__name__=~"db_couchbase.*"}` and click
  **Execute**.
- For Jaeger in Grafana, use **Explore → Jaeger** and search by service name
  `inventory-service`; do not rely on dashboard panels that may have stale
  time ranges.

### Port conflict on startup

If a port listed above is already in use, `docker compose up` will fail with
`address already in use`.  Stop the conflicting service or change the host-side
port mapping in `docker-compose.yml`:

```yaml
ports:
  - "19090:9090"   # change left side only; right side is the container port
```
