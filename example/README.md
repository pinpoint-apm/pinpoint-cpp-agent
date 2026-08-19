# Distributed tracing demo: proxy → server → MySQL

Two HTTP apps traced by the Pinpoint C++ agent:

- **`distributed_proxy`** (`cpp-proxy`, port 8080) — receives `GET /api/members`,
  injects the trace context, and forwards the call to the backend server.
- **`distributed_server`** (`cpp-db-server`, port 8081) — continues the received
  trace and answers the call with a **simulated** MySQL query, recorded as a
  MySQL span event, then hands follow-up work to a worker thread traced with
  an async span. The query is a stand-in so the demo needs no real database;
  in a real application, run the query inside the same span event and the
  tracing stays identical.

In the Pinpoint server map one request shows up as a single distributed trace:

```
client → cpp-proxy → cpp-db-server → MySQL
```

Both apps record the `User-Agent` request header (`...RECORD_REQUEST_HEADER`,
overridable through the environment; see [doc/config.md](../doc/config.md)).

## Run

Both apps build with the examples (`BUILD_EXAMPLES=ON`, the default) and have
no dependencies beyond the agent library itself:

```bash
cmake --preset default
cmake --build build/default --target distributed_proxy distributed_server
```

[`run.sh`](run.sh) starts both servers and stops them together on Ctrl-C. A
reachable Pinpoint collector is required (for a full stack see
[pinpoint-docker](https://github.com/pinpoint-apm/pinpoint-docker)):

```bash
PINPOINT_CPP_COLLECTOR_HOST=my-collector example/run.sh
```

Generate traffic:

```bash
curl http://localhost:8080/api/members
```

Then open the Pinpoint web UI and select the `cpp-proxy` application.

## Wiring

| Variable | Default | Used by |
|----------|---------|---------|
| `BACKEND` | `127.0.0.1:8081` | proxy — backend `host:port` |
| `PINPOINT_CPP_COLLECTOR_HOST` etc. | — | both — [agent configuration](../doc/config.md) |

## More examples

More examples are in the
[pinpoint-cpp-examples](https://github.com/pinpoint-apm/pinpoint-cpp-examples)
repository. For C applications, see the
[civetweb example](https://github.com/pinpoint-apm/pinpoint-cpp-examples/tree/main/civetweb)
and the
[nginx module](https://github.com/pinpoint-apm/pinpoint-cpp-examples/blob/main/nginx/ngx_http_pinpoint_module.c).
