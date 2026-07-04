/**
 * @file ptp_unc_ipc.h
 * @brief Shared memory layout for PTP time uncertainty data
 */
#ifndef PTP_UNC_IPC_H_
#define PTP_UNC_IPC_H_

#include <stdint.h>

#define PTP_UNC_SHM_NAME "/ptp_uncertainty"

/*
 * Seqlock-style update protocol:
 *   WRITER: seq=0 -> write fields -> seq=new (odd generation)
 *   READER: read seq -> read fields -> re-read seq; retry if changed or seq==0
 */
struct ptp_unc_shm_segment {
    uint32_t seq;
    uint32_t generation;

    uint32_t ptp4l_connected;
    uint32_t port_state;           /* IEEE 1588 port state (1-9) */
    uint32_t is_synchronized;      /* 1 when port_state == SLAVE */

    int64_t  offset_from_master_ns;
    int64_t  mean_path_delay_ns;
    uint16_t steps_removed;
    uint16_t reserved;

    uint32_t gm_present;
    uint8_t  gm_id[8];

    uint64_t total_uncertainty_ns; /* snapshot: |offset| + |mean_path_delay| */
    uint64_t update_timestamp_ns;  /* CLOCK_MONOTONIC at last successful poll */

    int64_t  master_offset_ns;     /* TIME_STATUS_NP offset at ingress_time */
    uint64_t ingress_time_ns;      /* TIME_STATUS_NP PHC timestamp of last sync */
    uint64_t ingress_mono_ns;      /* CLOCK_MONOTONIC when snapshot was taken */
    uint64_t max_drift_ppb;        /* worst-case drift bound for extrapolation */

    int32_t  phc_index;            /* from PORT_HWCLOCK_NP, -1 if unknown */
    uint32_t poll_interval_ms;
} __attribute__((aligned(64)));

#endif /* PTP_UNC_IPC_H_ */
