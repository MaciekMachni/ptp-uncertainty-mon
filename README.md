# ptp-uncertainty

Lightweight daemon and library that poll **ptp4l** for PTP synchronization status, publish the latest snapshot through shared memory, and let clients evaluate the current uncertainty with a configured drift bound.

The daemon talks to the `ptp4l` Unix management socket. Applications read `/ptp_uncertainty` through `libptp_unc.so`, so they do not need to send management messages themselves.

## What It Provides

`ptp_unc_dmn` collects:

- `offset_from_master_ns`, `mean_path_delay_ns`, and `steps_removed` from `CURRENT_DATA_SET`
- `port_state`, `is_synchronized`, and `logSyncInterval` from `PORT_DATA_SET`
- grandmaster presence/identity and sync ingress time from `TIME_STATUS_NP`
- optional `phc_index` from `PORT_HWCLOCK_NP`

The client library reports:

- `drift_ns`, the worst-case drift since the last sync ingress timestamp
- `total_uncertainty_ns = |offset_from_master_ns| + |mean_path_delay_ns| + drift_ns`
- `age_ns`, the monotonic age of the latest daemon update
- `ptp4l_connected`, which tells clients whether the daemon is currently connected to `ptp4l`

If `ptp4l` restarts or temporarily disconnects, the daemon preserves the last valid sync anchor and keeps publishing it with `ptp4l_connected=0`. Drift continues to grow from the preserved ingress timestamp until fresh sync data arrives.

## Architecture

```text
ptp4l  <--Unix socket-->  ptp_unc_dmn  -->  /ptp_uncertainty (SHM)  -->  libptp_unc.so  -->  apps
```

## Build

```bash
make
```

Outputs in `bin/`:

- `ptp_unc_dmn` - daemon
- `libptp_unc.so` - client library
- `watch_uncertainty` - example monitor

## Usage

```bash
# Start daemon. This usually requires root because the ptp4l socket is root-owned.
sudo ./bin/ptp_unc_dmn --ptp4l-socket /run/ptp4l -v

# Or start with a config file.
sudo ./bin/ptp_unc_dmn --config cfg/ptp_unc.conf.example

# Read extrapolated uncertainty once per second.
./bin/watch_uncertainty

# Poll more often from the client side.
./bin/watch_uncertainty 100

# Single extrapolated snapshot.
./bin/watch_uncertainty --once
```

## Daemon Options

```text
-s, --ptp4l-socket PATH   ptp4l management socket path
-p, --poll-interval MS    daemon poll interval; disables auto-polling
-d, --max-drift-ppb PPB   worst-case drift bound for extrapolation
-D, --domain NUM          PTP domain number
-A, --auto-polling        derive poll interval from logSyncInterval
    --phc-auto-detect     detect PHC index from PORT_HWCLOCK_NP
-c, --config PATH         configuration file
-v, --verbose             verbose daemon logging
```

Defaults:

- `ptp4l_socket`: `/var/run/ptp4l`
- `poll_interval_ms`: `1000`
- `max_drift_ppb`: `100000` (100 ppm)
- `ptp4l_domain`: `0`

## Configuration

See `cfg/ptp_unc.conf.example`:

```text
ptp4l_socket /var/run/ptp4l
poll_interval_ms 1000
ptp4l_auto_polling 1
phc_auto_detect 1
ptp4l_domain 0
max_drift_ppb 100000
verbose 0
```

When `ptp4l_auto_polling` is enabled and no manual poll interval is set, the daemon polls at twice the sync message rate reported by `ptp4l`.

## Client API

```c
#include "ptp_unc_api.h"

struct ptp_unc_handle *h = ptp_unc_open();
struct ptp_uncertainty u;

if (ptp_unc_get(h, &u) == 0) {
    /* u.total_uncertainty_ns, u.drift_ns, u.is_synchronized, etc. */
}

ptp_unc_close(h);
```

Use `ptp_unc_get()` to evaluate uncertainty at the current monotonic time. Use `ptp_unc_get_at()` when the caller already has a monotonic timestamp and wants uncertainty evaluated at that exact point.

## Requirements

- Linux with POSIX shared memory
- Running **linuxptp** `ptp4l` with the management socket enabled
- GCC and libc realtime support
