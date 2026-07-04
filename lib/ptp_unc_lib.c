/**
 * @file ptp_unc_lib.c
 * @brief Client library for reading PTP time uncertainty from shared memory
 */

#include "../dmn/ptp_unc_ipc.h"
#include "ptp_unc_api.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct ptp_unc_handle {
    struct ptp_unc_shm_segment *shm;
};

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t abs64(int64_t v)
{
    return (uint64_t)(v < 0 ? -v : v);
}

static int read_snapshot(struct ptp_unc_handle *h, struct ptp_unc_shm_segment *local)
{
    uint32_t seq1, seq2;
    int retries = 100;

    if (!h || !local || !h->shm)
        return -1;

    do {
        seq1 = h->shm->seq;
        if (seq1 == 0) {
            if (--retries <= 0)
                return -EAGAIN;
            continue;
        }

        __sync_synchronize();
        memcpy(local, h->shm, sizeof(*local));
        __sync_synchronize();
        seq2 = h->shm->seq;

        if (seq1 == seq2 && seq1 != 0)
            break;

        retries--;
    } while (retries > 0);

    if (seq1 != seq2 || seq1 == 0)
        return -EAGAIN;

    return 0;
}

static void extrapolate(const struct ptp_unc_shm_segment *local,
                        struct ptp_uncertainty *out,
                        uint64_t at_mono_ns)
{
    uint64_t offset_ns = abs64(local->offset_from_master_ns);
    uint64_t delay_ns = abs64(local->mean_path_delay_ns);
    uint64_t snapshot_total = offset_ns + delay_ns;
    uint64_t drift_ns = 0;

    if (at_mono_ns == 0)
        at_mono_ns = monotonic_ns();

    /*
     * Evaluate the current uncertainty by adding bounded clock drift to the
     * last ptp4l snapshot. Drift grows from the sync ingress timestamp.
     */
    if (local->max_drift_ppb > 0 &&
        local->ingress_time_ns > 0 &&
        local->ingress_mono_ns > 0 &&
        at_mono_ns > local->ingress_mono_ns) {
        uint64_t elapsed_ns = at_mono_ns - local->ingress_mono_ns;

        /* Guard against overflow of elapsed_ns * max_drift_ppb. */
        if (elapsed_ns > UINT64_MAX / local->max_drift_ppb)
            drift_ns = UINT64_MAX / 1000000000ULL;
        else
            drift_ns = (elapsed_ns * local->max_drift_ppb) / 1000000000ULL;
    }

    out->ptp4l_connected = local->ptp4l_connected;
    out->port_state = local->port_state;
    out->is_synchronized = local->is_synchronized;
    /*
     * Report both PTP contributors as magnitudes: they are the components
     * that accumulate into the worst-case (maximum) uncertainty bound, not
     * signed measurements.
     */
    out->offset_from_master_ns = (int64_t)offset_ns;
    out->mean_path_delay_ns = (int64_t)delay_ns;
    out->steps_removed = local->steps_removed;
    out->gm_present = local->gm_present;
    memcpy(out->gm_id, local->gm_id, 8);
    out->drift_ns = drift_ns;
    out->total_uncertainty_ns = snapshot_total + drift_ns;
    out->update_timestamp_ns = local->update_timestamp_ns;
    out->phc_index = local->phc_index;
    out->eval_mono_ns = at_mono_ns;
    out->max_drift_ppb = local->max_drift_ppb;

    if (local->update_timestamp_ns > 0 && at_mono_ns >= local->update_timestamp_ns)
        out->age_ns = at_mono_ns - local->update_timestamp_ns;
    else
        out->age_ns = 0;
}

struct ptp_unc_handle *ptp_unc_open(void)
{
    struct ptp_unc_handle *h;
    int fd;

    h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;

    fd = shm_open(PTP_UNC_SHM_NAME, O_RDONLY, 0);
    if (fd < 0) {
        free(h);
        return NULL;
    }

    h->shm = mmap(NULL, sizeof(*h->shm), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    if (h->shm == MAP_FAILED) {
        free(h);
        return NULL;
    }

    return h;
}

void ptp_unc_close(struct ptp_unc_handle *h)
{
    if (!h)
        return;
    if (h->shm && h->shm != MAP_FAILED)
        munmap(h->shm, sizeof(*h->shm));
    free(h);
}

int ptp_unc_get_at(struct ptp_unc_handle *h, struct ptp_uncertainty *out,
                   uint64_t at_mono_ns)
{
    struct ptp_unc_shm_segment local;
    int ret;

    if (!h || !out)
        return -1;

    ret = read_snapshot(h, &local);
    if (ret != 0)
        return ret;

    extrapolate(&local, out, at_mono_ns);
    return 0;
}

int ptp_unc_get(struct ptp_unc_handle *h, struct ptp_uncertainty *out)
{
    return ptp_unc_get_at(h, out, 0);
}
