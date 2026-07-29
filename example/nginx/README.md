# Pinpoint nginx module example

A minimal nginx dynamic module showing how to run the Pinpoint C++ agent
inside nginx's pre-fork worker model. It demonstrates the lifecycle contract
from the [Pre-fork Integration Guide](../../doc/prefork.md):

- the **master** process makes no agent API calls,
- each **worker** starts its own agent in `init_process`
  (`pt_start_agent()`; every worker registers as a distinct agent instance
  with its own auto-generated agent id),
- each worker shuts its agent down in `exit_process`,
- helper processes (cache manager/loader) are skipped,
- one span per completed request is recorded from the LOG phase, with the
  request's real start time and status, honoring incoming `Pinpoint-*`
  headers so nginx joins distributed traces started by its callers.

This is a **reference skeleton**, deliberately small. It is not built by this
repository's CMake/Bazel — it builds with nginx's own build system.

## Build

Build and install the agent library first (see the
[Build Guide](../../doc/build.md)), then build the module against the nginx
sources:

```bash
cd /path/to/nginx-src

./configure \
    --add-dynamic-module=/path/to/pinpoint-cpp-agent/example/nginx \
    --with-cc-opt="-I/usr/local/include" \
    --with-ld-opt="-L/usr/local/lib -lpinpoint_cpp"

make modules
cp objs/ngx_http_pinpoint_module.so /etc/nginx/modules/
```

## Configure

```nginx
load_module modules/ngx_http_pinpoint_module.so;

http {
    pinpoint_enable      on;
    pinpoint_config_file /etc/pinpoint/nginx-agent.yaml;

    # ... servers ...
}
```

`/etc/pinpoint/nginx-agent.yaml`:

```yaml
ApplicationName: "my-nginx"
AgentName: "my-nginx"          # shared display label; need not be unique
Collector:
  Host: "my.collector.host"
Log:
  # One log file per worker: the built-in rotation is not multi-process safe.
  FilePath: "/var/log/pinpoint/nginx-agent-%pid%.log"
```

Each worker appears as its own agent instance in the Pinpoint UI
(`worker_processes 4` → 4 instances). nginx config reloads (`SIGHUP`) and
binary upgrades (`SIGUSR2`) need no special handling: exiting workers shut
their agents down, new workers start fresh ones.

## Limitations of this skeleton

Kept out deliberately to stay readable; a production module would add:

- **Upstream context propagation** — inject `Pinpoint-*` headers into
  proxied requests (e.g. via `proxy_set_header` fed from module variables set
  in an early phase) so downstream services join the trace.
- **Copied header values** — the context reader returns pointers into
  nginx's header storage, which is NUL-terminated for HTTP/1 but not
  guaranteed for HTTP/2/3; copy values into `r->pool` for those.
- **Span events** for upstream calls, per-location URL templates for
  `pt_span_set_url_stat()`, and error detail recording.
