# Pre-fork Integration Guide

How to run the Pinpoint C++ agent inside pre-fork servers — hosts where a
master process `fork()`s a pool of worker processes that handle the actual
requests: nginx, Apache (prefork/event MPM), uWSGI, PHP-FPM-style engines, and
any custom master/worker design.

```
single-threaded master        — makes NO agent API calls
 ├─ fork → worker 1           — StartAgent() → traces requests
 ├─ fork → worker 2           — StartAgent() → traces requests
 └─ fork → worker N           — StartAgent() → traces requests
```

---

## The contract

**Every agent API call happens in the worker process.** The master process
must not call `StartAgent()` / `pt_start_agent()` — or any other agent
function — before forking.

This is the whole contract. Each worker builds, configures, starts and shuts
down its own agent, fully independent of its siblings:

- The worker's first gRPC use is also the process's first `grpc_init`, so no
  gRPC state ever crosses a `fork()` boundary (no `pthread_atfork` handlers or
  `GRPC_ENABLE_FORK_SUPPORT` needed). This assumes the **host application**
  also refrains from using gRPC before forking; if it does, the host must
  arrange its own gRPC fork handling.
- Each worker registers as its own agent instance with a process-unique agent
  id and its own start time.
- Each worker owns its own sender threads, queues, caches and collector
  connections. A worker crash or restart affects only that worker's tracing.

### What happens on misuse

The agent detects the most common mistake — starting an agent before
`fork()` — and degrades safely instead of crashing:

| Situation in the child | Behavior |
|---|---|
| Using an inherited started agent | `Enable()` returns `false` and `NewSpan()` returns noop spans (the pid guard sits on the span-creation path itself, so cached `AgentPtr`s and `GlobalAgent()` are covered alike), with a one-time error log. |
| Calling `Start()` on the inherited object | Refused with an error log. |
| `StartAgent()` while the inherited agent is installed | Refused (returns `false`) and the inherited agent is evicted from the singleton, so `GlobalAgent()` degrades to the noop agent: gRPC was initialized pre-fork in the parent, so a fresh agent cannot be built safely in this process. |
| Destroying / shutting down the inherited agent | Safe: inherited dead thread handles are abandoned, never joined. |

A worker that itself `fork()+exec()`s subprocesses (e.g. spawning tools) is
unaffected — the misuse case is only `fork()` without `exec()` followed by
agent use in the child.

---

## Worker identity

Each worker appears as one agent instance in the Pinpoint UI. Identity is
resolved inside the worker, when `StartAgent()` runs: the agent id is always
auto-generated (a fresh UUIDv7 per worker process), so sibling workers can
never collide — but the id changes on worker restart.

`AgentName` provides the human-readable display label. It need not be unique:
all workers can share one configured name (the auto-generated id already
tells the instances apart).

## Logging

Multiple workers writing one log file is safe at line granularity (every line
is flushed as a single append), but the built-in **size rotation is not
multi-process safe**: a worker that rotates the file pulls it out from under
its siblings. Give each worker its own file with the `Log.FilePath`
placeholder:

```yaml
Log:
  FilePath: "/var/log/pinpoint/agent-%pid%.log"
  MaxFileSize: 50
```

`%pid%` expands to the process id.

## Configuration reloads

With `EnableConfigFileWatcher: true` (default: false), each worker installs
its own config-file watcher on `AgentOptions::config_file_path`, so editing
the YAML file reconfigures every worker within the poll interval (default 1s)
— no host reload required. Identity and collector settings are not
reloadable; see the [Configuration Guide](config.md#configuration-hot-reload).

For a host-driven reload (e.g. nginx `SIGHUP`), nothing special is needed: old
workers shut their agents down in their exit hooks, and the newly forked
workers start fresh agents.

## Worker shutdown

Call `Shutdown()` / `pt_agent_shutdown()` from the worker's exit hook. It
drains queued spans to the collector (bounded wait) and joins the agent's
threads, returning within a 3-second deadline even if the collector is
unresponsive. A worker killed without the hook simply loses whatever was
still queued in that process.

## Operational notes

- **Instance count**: N workers = N agent instances on the collector and in
  the UI. Size `worker_processes` (and the collector) with that in mind.
- **Threads**: each worker runs the agent's background threads (~10) plus
  gRPC's internal threads. They are started with all signals blocked, so
  process-directed signals (nginx's `SIGTERM`/`SIGQUIT`/`SIGHUP` to a worker)
  are always delivered to the host's own threads, never to an agent thread.
- **Collector connections**: each worker maintains its own gRPC channels.
  Registration retries and reconnects are jittered per worker, so a collector
  outage does not produce synchronized retry storms across the pool.
- **Sampling is per worker**: `CounterRate: 10` samples 1-in-10 *per worker*.
  Rate-style expectations hold globally, but exact counter semantics do not
  span workers.

---

## nginx recipe

The lifecycle for a dynamic module is:

| nginx hook | Runs in | What to do |
|---|---|---|
| `init_module` | master | Nothing agent-related (parse directives only). |
| `init_process` | each worker | `pt_start_agent()` — only when `ngx_process` is `NGX_PROCESS_WORKER` or `NGX_PROCESS_SINGLE`; skip helper processes (cache manager/loader). |
| `exit_process` | each worker | `pt_agent_shutdown()` + `pt_agent_destroy()`. |

```c
/* init_process (worker) */
static ngx_int_t ngx_http_pinpoint_init_process(ngx_cycle_t *cycle) {
    if (ngx_process != NGX_PROCESS_WORKER && ngx_process != NGX_PROCESS_SINGLE) {
        return NGX_OK;  /* cache manager / loader: no agent */
    }

    pt_agent_options_t opts = pt_agent_options_new();
    pt_agent_options_set_config_file(opts, conf->config_file);
    pt_agent_options_set_server_metadata(opts, "nginx/" NGINX_VERSION,
                                         NULL, 0, NULL, 0);

    if (pt_start_agent(opts)) {
        g_agent = pt_global_agent();
    } else {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "pinpoint: agent start failed; worker runs untraced "
                      "(check the pinpoint agent log for the cause)");
    }
    pt_agent_options_free(opts);
    return NGX_OK;
}

/* exit_process (worker) */
static void ngx_http_pinpoint_exit_process(ngx_cycle_t *cycle) {
    if (g_agent != NULL) {
        pt_agent_shutdown(g_agent);
        pt_agent_destroy(g_agent);
        g_agent = NULL;
    }
}
```

Notes:

- `pt_start_agent()` returns quickly (registration is asynchronous), so worker
  startup latency is unaffected; `pt_agent_is_enabled()` flips to non-zero once
  the collector accepted the registration.
- nginx binary upgrades (`SIGUSR2`) `exec()` a fresh master, and config reloads
  (`SIGHUP`) fork fresh workers — both are covered by the per-worker lifecycle
  above with no extra handling.
- The same recipe applies to Apache (`ChildInit` hook), uWSGI
  (`postfork` hook), and any custom master/worker design.

## Other process models

- **Single-process applications** need nothing special: call `StartAgent()` at
  startup as shown in the [Quick Start Guide](quick_start.md).
- **Daemonizing hosts** (fork + `setsid` at startup) are fine as long as
  `StartAgent()` runs after the daemonization fork.
- **A traced worker forking a grandchild** that keeps running without `exec()`
  cannot trace: the grandchild must not reuse the inherited agent (it will be
  refused, see above), and gRPC cannot be re-initialized there. Trace in the
  process generation that called `StartAgent()`.
