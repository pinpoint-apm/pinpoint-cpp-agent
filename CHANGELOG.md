# Changelog

## Unreleased

### Breaking

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
