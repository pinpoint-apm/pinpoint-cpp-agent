# Changelog

## Unreleased

### Breaking

- **`Sampling.PercentRate: 0` now samples nothing.**

  Up to and including v2.0.0, `make_config()` raised any non-negative
  `PercentRate` below `0.01` — exactly `0` included — to `0.01`, so a
  deployment that configured `0` kept collecting traces at 0.01%. That floor is
  gone ([src/config.cpp:795-824](src/config.cpp#L795-L824)): `0` and below now
  disable percent sampling outright, and a positive rate below `0.01` (e.g.
  `0.005`) truncates to `0` and disables it too, with a warning.

  This matches the Java agent, where `parseSamplingRate` truncates the
  configured rate and `createSampler` hands a non-positive result to
  `FalseSampler` (`PercentSamplerFactory.java:40-48,56-58`). The other two
  outcomes were already in place: `>= 100` is always-sample (Java's
  `TrueSampler`) and everything between runs the percent sampler.

  **Symptom if you do not migrate:** with `Sampling.Type: PERCENT` and
  `PercentRate` at `0` (or below `0.01`), the agent stops sampling new
  transactions — no new traces appear in the UI. Continued traces and
  throughput limiting are unaffected, as is `Sampling.Type: COUNTER`.

  **Migration.** A deployment that relied on `0` meaning 0.01% must say so:
  set `PercentRate: 0.01` (YAML) or `PINPOINT_CPP_SAMPLING_PERCENT_RATE=0.01`.
  Deployments that meant `0` as "off" need no change — they now get what they
  asked for.

  See [Sampling Configuration](doc/config.md#sampling-configuration).

- **`ServiceName` is now required for `UidVersion: v4`.**

  Up to and including v2.0.0, a v4 agent started with no `ServiceName`
  silently registered under the fallback service name `DEFAULT` (mirroring
  Java's `ServiceUid.DEFAULT_SERVICE_UID_NAME`). That fallback is gone:
  `resolve_object_name()` now returns `std::nullopt` for a missing or invalid
  `ServiceName`, and the caller aborts agent startup
  ([src/object_name.cpp:211-218](src/object_name.cpp#L211-L218),
  [src/object_name.h:126-132](src/object_name.h#L126-L132)). This matches Java's
  `ObjectNameResolverV4` ("ServiceName not provided") and the Go agent.

  **Symptom if you do not migrate:** the process does **not** start — this is a
  startup failure, not a silent degradation to reduced tracing. The log shows:

  ```
  Failed to resolve ServiceName (required for uid.version=v4, max length 254)
  ```

  **Migration.** Any deployment running `UidVersion: v4` without a
  `ServiceName` must do one of:

  - Set the service name explicitly — YAML key `ServiceName`, or environment
    variable `PINPOINT_CPP_SERVICE_NAME` (max 254 bytes, `[a-zA-Z0-9._-]`). Use
    `DEFAULT` to keep registering under exactly the service the v2.0.0 fallback
    produced.
  - Or stay on the v1/v3 identity, which does not use `ServiceName` at all —
    YAML key `UidVersion: v3`, or `PINPOINT_CPP_UID_VERSION=v3` (`v3` is the
    default).

  See [Identity Versions](doc/config.md#identity-versions).
