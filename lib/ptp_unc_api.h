/**
 * @file ptp_unc_api.h
 * @brief Client API for reading PTP time uncertainty from shared memory
 */
#ifndef PTP_UNC_API_H_
#define PTP_UNC_API_H_

#include <stdint.h>

#define PTP_PORT_STATE_SLAVE 9

struct ptp_unc_handle;

struct ptp_uncertainty {
    uint32_t ptp4l_connected;
    uint32_t port_state;
    uint32_t is_synchronized;

    int64_t  offset_from_master_ns;
    int64_t  mean_path_delay_ns;
    uint16_t steps_removed;

    uint32_t gm_present;
    uint8_t  gm_id[8];

    uint64_t total_uncertainty_ns; /* |offset| + |delay| + drift_ns */
    uint64_t update_timestamp_ns;
    uint64_t age_ns;               /* monotonic age since last daemon update */
    uint64_t drift_ns;             /* drift component added by extrapolation */
    uint64_t eval_mono_ns;         /* monotonic time uncertainty was evaluated at */
    uint64_t max_drift_ppb;        /* drift bound from daemon (ppb) */

    int32_t  phc_index;
};

struct ptp_unc_handle *ptp_unc_open(void);
void ptp_unc_close(struct ptp_unc_handle *h);
int  ptp_unc_get(struct ptp_unc_handle *h, struct ptp_uncertainty *out);
int  ptp_unc_get_at(struct ptp_unc_handle *h, struct ptp_uncertainty *out,
                    uint64_t at_mono_ns);

#endif /* PTP_UNC_API_H_ */
