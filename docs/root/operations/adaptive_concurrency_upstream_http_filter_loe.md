# Level of effort: adaptive concurrency as an upstream HTTP filter

## Current state (as of this branch)

`envoy.filters.http.adaptive_concurrency` is currently registered only as a downstream HTTP filter (`NamedHttpFilterConfigFactory`) and is **not** registered as an upstream HTTP filter (`UpstreamHttpFilterConfigFactory`).

This means it cannot be referenced in `router.upstream_http_filters` today.

## Why this matters

The router's upstream HTTP chain resolves factories using upstream filter interfaces and expects the chain to terminate in `envoy.filters.http.upstream_codec`.

So enabling adaptive concurrency in upstream chains requires:

1. registering an upstream factory, and
2. validating chain placement/behavior with the existing upstream codec terminal requirements.

## LOE estimate

### Small/Medium implementation (about 1-2 engineering days)

- Add upstream filter registration for adaptive concurrency.
- Ensure config path works with `UpstreamFactoryContext`.
- Add/adjust unit coverage for factory wiring and basic config validation.

### Medium verification and hardening (about 2-4 engineering days)

- Add integration test coverage in upstream filter integration tests for:
  - successful request flow,
  - missing `upstream_codec` failure behavior,
  - interaction with runtime enable/disable gating.
- Validate stats prefix behavior for upstream filter chains (`upstream_http_filter` prefix expectations).

### Potential follow-up (optional, 1-2 days)

- Documentation updates to indicate supported upstream use explicitly.
- Extension metadata updates (if exposing upstream category/status for this extension).

## Risks / unknowns

- The filter was designed around latency signals and request admission semantics; behavior in upstream filter position should be reviewed for expected control-loop signal quality.
- Retry/hedging semantics for local replies from upstream filters may affect perceived behavior and should be explicitly tested.

## Recommendation

Treat this as a **medium LOE** change overall: implementation is straightforward, but confidence depends on integration tests and runtime-behavior validation.
