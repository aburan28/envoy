# RFC: Adaptive Concurrency as an Upstream HTTP Filter

## Summary

This proposal adds upstream HTTP filter support to the existing adaptive concurrency filter. Today, the adaptive concurrency filter (`envoy.filters.http.adaptive_concurrency`) operates only as a downstream HTTP filter, applying a single concurrency limit across all upstream clusters. Moving it to an upstream filter enables **per-cluster** and **per-connection-pool** concurrency control, aligning the concurrency limit with the actual upstream it protects.

## Problem Statement

The current downstream adaptive concurrency filter has a fundamental placement limitation: it sits in the downstream filter chain and makes forwarding decisions **before** a cluster is selected by the router. This means:

1. **No per-cluster granularity** — A single concurrency limit is shared across all upstream clusters. A slow upstream pollutes the concurrency budget for healthy upstreams.
2. **Latency signal mismatch** — The latency samples include downstream filter chain overhead, not just upstream round-trip time. This biases the gradient controller's minRTT and sampleRTT calculations.
3. **No cluster-level configuration** — Operators cannot tune concurrency parameters (e.g., `max_concurrency_limit`, `concurrency_update_interval`, `min_rtt_calc_params`) per cluster.
4. **Interaction with retries** — When placed downstream, requests blocked by the concurrency limit cannot be retried by the router since the router hasn't been invoked yet.

An upstream HTTP filter placement resolves all of these issues by running the concurrency control logic in the per-cluster upstream filter chain, after routing decisions are made.

## Goals

- [ ] Convert the adaptive concurrency filter to a dual filter (supporting both downstream and upstream placement)
- [ ] Ensure the gradient controller operates per-cluster when configured as an upstream filter
- [ ] Handle upstream-specific concerns: hedging, retries, shadowed/mirrored requests, and `sendLocalReply` semantics
- [ ] Preserve full backward compatibility with existing downstream filter configuration
- [ ] Add integration tests for the upstream filter path

## Non-Goals

- Changing the gradient controller algorithm itself
- Per-host (as opposed to per-cluster) concurrency limiting (future work)
- Deprecating the downstream filter placement

## Design

### 1. Factory Conversion: `FactoryBase` to `DualFactoryBase`

Following the [upstream filter conversion guide](../../source/docs/upstream_filters.md), the factory will be converted from `FactoryBase` to `DualFactoryBase`:

```cpp
// config.h
class AdaptiveConcurrencyFilterFactory
    : public Common::DualFactoryBase<
          envoy::extensions::filters::http::adaptive_concurrency::v3::AdaptiveConcurrency> {
public:
  AdaptiveConcurrencyFilterFactory()
      : DualFactoryBase("envoy.filters.http.adaptive_concurrency") {}

  Http::FilterFactoryCb createFilterFactoryFromProtoTyped(
      const envoy::extensions::filters::http::adaptive_concurrency::v3::AdaptiveConcurrency&,
      const std::string&, DualInfo, Server::Configuration::ServerFactoryContext&) override;
};

using UpstreamAdaptiveConcurrencyFilterFactory = AdaptiveConcurrencyFilterFactory;
```

```cpp
// config.cc
REGISTER_FACTORY(UpstreamAdaptiveConcurrencyFilterFactory,
                 Server::Configuration::UpstreamHttpFilterConfigFactory);
```

The `DualInfo` struct provides the correct `init_manager` and `scope` for both downstream and upstream contexts, which is critical for xDS reload correctness.

### 2. Controller Lifecycle: Per-Cluster Scoping

When the filter runs upstream, the `GradientController` instance must be **scoped to the cluster**, not shared globally. This happens naturally because:

- Upstream filter factories are instantiated per-cluster (via `HttpProtocolOptions`) or per-router-config
- Each factory invocation creates its own `GradientController` via the factory lambda
- Stats are scoped to the cluster's `Stats::Scope`, giving per-cluster visibility (e.g., `cluster.my_cluster.adaptive_concurrency.gradient`)

No changes to the controller itself are needed. The per-cluster scoping falls out of the upstream filter chain architecture.

### 3. `sendLocalReply` Behavior in Upstream Context

The current filter calls `sendLocalReply` when the concurrency limit is exceeded. In the upstream context, this has different semantics:

- **Local replies from upstream filters do NOT trigger retries.** The response is treated as final.
- **For hedged requests**, a local reply from one hedge attempt counts as the final response.

This is actually **desirable** for concurrency limiting — if a cluster is overloaded, retrying against the same cluster would worsen the problem. However, this should be clearly documented.

**Open question:** Should we add an option to return a specific retry-triggering status code (e.g., via `x-envoy-overloaded`) so that operators can configure retry policies to route to a different cluster? This could be a follow-up enhancement.

### 4. Health Check Filtering

The current filter skips health check requests via `decoder_callbacks_->streamInfo().healthCheck()`. This check remains valid in the upstream context — active health checks do not go through the upstream filter chain, and passive health check traffic (if marked) should still be excluded from latency sampling.

### 5. Shadowed/Mirrored Requests

Per the upstream filter documentation, the downstream connection is not available for mirrored/shadowed requests. The adaptive concurrency filter does not access the downstream connection, so no changes are needed. However, mirrored requests **will** count against the concurrency limit and contribute latency samples, which is the correct behavior — they consume upstream resources.

### 6. Hedging Interaction

When hedging is enabled, multiple upstream filter instances may operate in parallel against the same `StreamInfo`. The adaptive concurrency filter:

- Reads `streamInfo().healthCheck()` (const access — safe)
- Uses its own `deferred_sample_task_` member (per-filter instance — safe)
- Calls controller methods which are documented as thread-safe

No hedging-specific changes are needed.

### 7. Configuration

#### Cluster-level (recommended for upstream use)

```yaml
clusters:
- name: my_cluster
  typed_extension_protocol_options:
    envoy.extensions.upstreams.http.v3.HttpProtocolOptions:
      "@type": type.googleapis.com/envoy.extensions.upstreams.http.v3.HttpProtocolOptions
      explicit_http_config:
        http_protocol_options: {}
      http_filters:
      - name: envoy.filters.http.adaptive_concurrency
        typed_config:
          "@type": type.googleapis.com/envoy.extensions.filters.http.adaptive_concurrency.v3.AdaptiveConcurrency
          gradient_controller_config:
            sample_aggregate_percentile:
              value: 90
            concurrency_limit_params:
              concurrency_update_interval: 0.1s
            min_rtt_calc_params:
              interval: 60s
              request_count: 50
      - name: envoy.filters.http.upstream_codec
        typed_config:
          "@type": type.googleapis.com/envoy.extensions.filters.http.upstream_codec.v3.UpstreamCodec
```

#### Router-level (applies to all clusters using this router)

```yaml
http_filters:
- name: envoy.filters.http.router
  typed_config:
    "@type": type.googleapis.com/envoy.extensions.filters.http.router.v3.Router
    upstream_http_filters:
    - name: envoy.filters.http.adaptive_concurrency
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.filters.http.adaptive_concurrency.v3.AdaptiveConcurrency
        gradient_controller_config:
          sample_aggregate_percentile:
            value: 90
          concurrency_limit_params:
            concurrency_update_interval: 0.1s
          min_rtt_calc_params:
            interval: 60s
            request_count: 50
    - name: envoy.filters.http.upstream_codec
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.filters.http.upstream_codec.v3.UpstreamCodec
```

No proto changes are required — the existing `AdaptiveConcurrency` message works unchanged in both downstream and upstream contexts.

## Implementation Plan

### Phase 1: Dual Filter Conversion

1. Convert `AdaptiveConcurrencyFilterFactory` from `FactoryBase` to `DualFactoryBase`
2. Register `UpstreamAdaptiveConcurrencyFilterFactory`
3. Add `envoy.filters.http.upstream` to `extensions_metadata.yaml`
4. Update `DualInfo` usage for `init_manager` and `scope`

### Phase 2: Testing

1. Unit tests for upstream filter factory creation
2. Integration tests:
   - Basic upstream concurrency limiting (request blocked at limit)
   - Gradient controller convergence with upstream latency
   - Interaction with retries (verify no retry on concurrency rejection)
   - Per-cluster isolation (two clusters with different load profiles)

### Phase 3: Documentation

1. Update `adaptive_concurrency_filter.rst` with upstream usage examples
2. Document `sendLocalReply` / retry interaction
3. Add per-cluster configuration examples

## Open Questions

1. **Per-host concurrency limiting**: Should we support per-upstream-host concurrency limits (in addition to per-cluster)? This would require a controller instance per host, likely managed by the cluster's host set. Deferring to future work.

2. **Retry interaction**: Should concurrency-limited responses be retryable to a different cluster (e.g., via retry plugins or `x-envoy-overloaded`)? The default behavior (no retry) is safe, but operators may want this flexibility.

3. **Stats prefix**: When running as an upstream filter, stats will be scoped under the cluster prefix. Should we maintain the same stat names for downstream compatibility, or add an `upstream.` prefix to distinguish?

## References

- [Existing adaptive concurrency filter](../../source/extensions/filters/http/adaptive_concurrency/)
- [Upstream filter conversion guide](../../source/docs/upstream_filters.md)
- [Buffer filter dual conversion (PR #23071)](https://github.com/envoyproxy/envoy/pull/23071)
- [Netflix Gradient algorithm](https://medium.com/netflix-techblog/performance-under-load-3e6fa9a60581)
- [Upstream HTTP filter chain architecture](../../source/common/router/upstream_request.h)
