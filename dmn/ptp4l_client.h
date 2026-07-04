/**
 * @file ptp4l_client.h
 * @brief PTP4L management socket client for time uncertainty queries
 */
#ifndef PTP4L_CLIENT_H_
#define PTP4L_CLIENT_H_

#include <stdint.h>
#include <time.h>

#define PTP4L_DEFAULT_SOCKET   "/var/run/ptp4l"
#define PTP4L_MSG_MAX_SIZE     1024
#define PTP4L_RECONNECT_INTERVAL 5

#define PTP_MGMT_MSG_TYPE      0x0D
#define PTP_MGMT_GET           0x00
#define PTP_MGMT_RESPONSE      0x02

#define PTP_MGMT_CURRENT_DATA_SET 0x2001
#define PTP_MGMT_PORT_DATA_SET    0x2004
#define PTP_MGMT_TIME_STATUS_NP   0xC000
#define PTP_MGMT_PORT_HWCLOCK_NP  0xC009

struct ptp_header {
    uint8_t  messageType;
    uint8_t  versionPTP;
    uint16_t messageLength;
    uint8_t  domainNumber;
    uint8_t  reserved1;
    uint16_t flagField;
    uint64_t correctionField;
    uint32_t reserved2;
    uint8_t  sourcePortIdentity[10];
    uint16_t sequenceId;
    uint8_t  controlField;
    int8_t   logMessageInterval;
} __attribute__((packed));

struct ptp_mgmt_msg {
    struct ptp_header header;
    uint8_t  targetPortIdentity[10];
    uint8_t  startingBoundaryHops;
    uint8_t  boundaryHops;
    uint8_t  actionField;
    uint8_t  reserved;
} __attribute__((packed));

struct ptp_mgmt_tlv {
    uint16_t tlvType;
    uint16_t lengthField;
    uint16_t managementId;
    uint8_t  data[];
} __attribute__((packed));

struct ptp_current_data_set {
    uint16_t stepsRemoved;
    int64_t  offsetFromMaster;
    int64_t  meanPathDelay;
} __attribute__((packed));

struct ptp_port_data_set {
    uint8_t portIdentity[10];
    uint8_t portState;
    int8_t  logMinDelayReqInterval;
    uint8_t peerMeanPathDelay[8];
    int8_t  logAnnounceInterval;
    uint8_t announceReceiptTimeout;
    int8_t  logSyncInterval;
    uint8_t delayMechanism;
    int8_t  logMinPdelayReqInterval;
    uint8_t versionNumber;
} __attribute__((packed));

struct ptp_scaled_ns {
    uint16_t nanoseconds_msb;
    uint64_t nanoseconds_lsb;
    uint16_t fractional_nanoseconds;
} __attribute__((packed));

struct ptp_time_status_np {
    int64_t  master_offset;
    int64_t  ingress_time;
    int32_t  cumulativeScaledRateOffset;
    int32_t  scaledLastGmPhaseChange;
    uint16_t gmTimeBaseIndicator;
    struct ptp_scaled_ns lastGmPhaseChange;
    int32_t  gmPresent;
    uint8_t  gmIdentity[8];
} __attribute__((packed));

struct ptp_port_hwclock_np {
    uint8_t portIdentity[10];
    int32_t phc_index;
    uint8_t flags;
    uint8_t reserved;
} __attribute__((packed));

struct ptp4l_client {
    int      socket_fd;
    char    *socket_path;
    time_t   last_connect_attempt;
    int      connected;
    uint16_t sequence_id;
    uint8_t  domain_number;
    int      verbose;

    struct ptp_current_data_set current_data;
    struct ptp_port_data_set    port_data;
    struct ptp_time_status_np   time_status;
    struct ptp_port_hwclock_np  hwclock_data;

    int current_data_valid;
    int port_data_valid;
    int time_status_valid;
    int hwclock_data_valid;
};

void ptp4l_client_init(struct ptp4l_client *c, const char *socket_path,
                       uint8_t domain_number, int verbose);
void ptp4l_client_cleanup(struct ptp4l_client *c);
int  ptp4l_client_connect(struct ptp4l_client *c);
void ptp4l_client_disconnect(struct ptp4l_client *c);
int  ptp4l_client_is_readonly_socket(const char *socket_path);
int  ptp4l_client_get_current_data_set(struct ptp4l_client *c);
int  ptp4l_client_get_port_data_set(struct ptp4l_client *c);
int  ptp4l_client_get_time_status_np(struct ptp4l_client *c);
int  ptp4l_client_get_port_hwclock_np(struct ptp4l_client *c, uint16_t port_number);
int  ptp4l_client_auto_detect_phc(struct ptp4l_client *c, int32_t *phc_index);
int  ptp4l_client_query_all(struct ptp4l_client *c);

#endif /* PTP4L_CLIENT_H_ */
