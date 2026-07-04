/**
 * @file watch_uncertainty.c
 * @brief Example client that prints PTP time uncertainty from shared memory
 */

#include "../lib/ptp_unc_api.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(int ms)
{
    struct timespec req;

    if (ms <= 0)
        return;

    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;

    while (nanosleep(&req, &req) != 0 && errno == EINTR)
        ;
}

static const char *port_state_name(unsigned state)
{
    static const char *names[] = {
        "UNKNOWN", "INITIALIZING", "FAULTY", "DISABLED", "LISTENING",
        "PRE_MASTER", "MASTER", "PASSIVE", "UNCALIBRATED", "SLAVE"
    };

    if (state <= 9)
        return names[state];
    return "UNKNOWN";
}

static void print_gm_id(const uint8_t id[8])
{
    printf("%02x%02x%02x.%02x%02x.%02x%02x%02x",
           id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--once] [INTERVAL_MS]\n"
            "  INTERVAL_MS  Poll interval in milliseconds (default: 1000)\n"
            "  --once       Print one extrapolated snapshot and exit\n",
            prog);
}

int main(int argc, char **argv)
{
    struct ptp_unc_handle *h;
    struct ptp_uncertainty u;
    int interval_ms = 1000;
    int once = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            interval_ms = atoi(argv[i]);
            if (interval_ms <= 0) {
                fprintf(stderr, "Invalid interval: %s\n", argv[i]);
                usage(argv[0]);
                return 1;
            }
        }
    }

    h = ptp_unc_open();
    if (!h) {
        fprintf(stderr, "Cannot open shared memory. Is ptp_unc_dmn running?\n");
        return 1;
    }

    do {
        if (ptp_unc_get(h, &u) != 0) {
            printf("waiting for daemon update...\n");
        } else {
            printf("ptp4l_connected=%u synchronized=%u port=%s (%u)\n",
                   u.ptp4l_connected, u.is_synchronized,
                   port_state_name(u.port_state), u.port_state);
            printf("offset_from_master=%ld ns\n", (long)u.offset_from_master_ns);
            printf("mean_path_delay=%ld ns\n", (long)u.mean_path_delay_ns);
            printf("drift=%lu ns (max_drift=%lu ppb)\n",
                   (unsigned long)u.drift_ns,
                   (unsigned long)u.max_drift_ppb);
            printf("total_uncertainty=%lu ns\n",
                   (unsigned long)u.total_uncertainty_ns);
            printf("steps_removed=%u snapshot_age=%lu ms\n",
                   u.steps_removed,
                   (unsigned long)(u.age_ns / 1000000ULL));
            if (u.gm_present) {
                printf("gm_id=");
                print_gm_id(u.gm_id);
                printf("\n");
            }
            if (u.phc_index >= 0)
                printf("phc_index=%d (/dev/ptp%d)\n", u.phc_index, u.phc_index);
            printf("---\n");
        }

        if (once)
            break;
        sleep_ms(interval_ms);
    } while (1);

    ptp_unc_close(h);
    return 0;
}
