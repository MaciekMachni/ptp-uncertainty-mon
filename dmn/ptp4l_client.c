/**
 * @file ptp4l_client.c
 * @brief PTP4L management socket client implementation
 */

#include "byteorder.h"
#include "ptp4l_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void log_msg(struct ptp4l_client *c, const char *fmt, ...)
{
    va_list ap;

    if (!c->verbose)
        return;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static int send_mgmt(struct ptp4l_client *c, uint16_t mgmt_id, uint8_t action,
                     uint16_t port_number)
{
    uint8_t buffer[PTP4L_MSG_MAX_SIZE];
    struct ptp_mgmt_msg *msg = (struct ptp_mgmt_msg *)buffer;
    struct ptp_mgmt_tlv *tlv;
    size_t msg_len;

    if (!c->connected)
        return -1;

    memset(buffer, 0, sizeof(buffer));

    msg->header.messageType = PTP_MGMT_MSG_TYPE;
    msg->header.versionPTP = 2;
    msg->header.domainNumber = c->domain_number;
    msg->header.sequenceId = htons(c->sequence_id++);
    msg->header.controlField = 4;
    msg->header.logMessageInterval = 0x7F;

    if (port_number == 0) {
        memset(msg->targetPortIdentity, 0xFF, sizeof(msg->targetPortIdentity));
    } else {
        memset(msg->targetPortIdentity, 0xFF, 8);
        msg->targetPortIdentity[8] = (port_number >> 8) & 0xFF;
        msg->targetPortIdentity[9] = port_number & 0xFF;
    }

    msg->actionField = action;

    tlv = (struct ptp_mgmt_tlv *)(buffer + sizeof(*msg));
    tlv->tlvType = htons(0x0001);
    tlv->lengthField = htons(2);
    tlv->managementId = htons(mgmt_id);

    msg_len = sizeof(*msg) + sizeof(*tlv);
    msg->header.messageLength = htons(msg_len);

    if (send(c->socket_fd, buffer, msg_len, 0) < 0) {
        perror("ptp4l send");
        ptp4l_client_disconnect(c);
        return -1;
    }

    return 0;
}

static int recv_response(struct ptp4l_client *c, uint8_t *buffer, size_t size)
{
    fd_set rfds;
    struct timeval tv;
    int ret;

    if (!c->connected)
        return -1;

    FD_ZERO(&rfds);
    FD_SET(c->socket_fd, &rfds);
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    ret = select(c->socket_fd + 1, &rfds, NULL, NULL, &tv);
    if (ret <= 0)
        return ret;

    ret = recv(c->socket_fd, buffer, size, 0);
    if (ret < 0) {
        perror("ptp4l recv");
        ptp4l_client_disconnect(c);
    }
    return ret;
}

static int parse_response(struct ptp4l_client *c, uint8_t *buffer, size_t len)
{
    struct ptp_mgmt_msg *msg;
    struct ptp_mgmt_tlv *tlv;
    uint16_t mgmt_id;
    uint16_t tlv_len;
    size_t payload_len;
    uint8_t *data;

    if (len < sizeof(*msg))
        return -1;

    msg = (struct ptp_mgmt_msg *)buffer;
    if (msg->header.messageType != PTP_MGMT_MSG_TYPE ||
        msg->actionField != PTP_MGMT_RESPONSE)
        return -1;

    if (len < sizeof(*msg) + sizeof(*tlv))
        return -1;

    tlv = (struct ptp_mgmt_tlv *)(buffer + sizeof(*msg));
    mgmt_id = ntohs(tlv->managementId);
    tlv_len = ntohs(tlv->lengthField);
    data = tlv->data;

    /*
     * Bytes actually available for the management payload. The TLV
     * lengthField covers managementId (2 bytes) plus the payload, so the
     * usable payload is bounded by what was really received, never by the
     * attacker/wire-controlled lengthField alone.
     */
    payload_len = len - (sizeof(*msg) + sizeof(*tlv));
    if (tlv_len < 2)
        return -1;
    if ((size_t)(tlv_len - 2) < payload_len)
        payload_len = (size_t)(tlv_len - 2);

    switch (mgmt_id) {
    case PTP_MGMT_CURRENT_DATA_SET:
        if (payload_len >= sizeof(struct ptp_current_data_set)) {
            memcpy(&c->current_data, data, sizeof(c->current_data));
            c->current_data.stepsRemoved = ntohs(c->current_data.stepsRemoved);
            c->current_data.offsetFromMaster = be64toh(c->current_data.offsetFromMaster);
            c->current_data.meanPathDelay = be64toh(c->current_data.meanPathDelay);
            c->current_data_valid = 1;
            return 0;
        }
        break;

    case PTP_MGMT_PORT_DATA_SET:
        if (payload_len >= sizeof(struct ptp_port_data_set)) {
            memcpy(&c->port_data, data, sizeof(c->port_data));
            c->port_data_valid = 1;
            return 0;
        }
        break;

    case PTP_MGMT_TIME_STATUS_NP:
        if (payload_len >= sizeof(struct ptp_time_status_np)) {
            memcpy(&c->time_status, data, sizeof(c->time_status));
            c->time_status.master_offset =
                be64toh(c->time_status.master_offset);
            c->time_status.ingress_time =
                be64toh(c->time_status.ingress_time);
            c->time_status.cumulativeScaledRateOffset =
                be32toh(c->time_status.cumulativeScaledRateOffset);
            c->time_status.scaledLastGmPhaseChange =
                be32toh(c->time_status.scaledLastGmPhaseChange);
            c->time_status.gmTimeBaseIndicator =
                be16toh(c->time_status.gmTimeBaseIndicator);
            c->time_status.gmPresent = be32toh(c->time_status.gmPresent);
            c->time_status_valid = 1;
            return 0;
        }
        break;

    case PTP_MGMT_PORT_HWCLOCK_NP:
        if (payload_len >= sizeof(struct ptp_port_hwclock_np)) {
            memcpy(&c->hwclock_data, data, sizeof(c->hwclock_data));
            c->hwclock_data.phc_index = be32toh(c->hwclock_data.phc_index);
            c->hwclock_data_valid = 1;
            return 0;
        }
        break;
    }

    return -1;
}

static int query_tlv(struct ptp4l_client *c, uint16_t mgmt_id, uint16_t port)
{
    uint8_t buf[PTP4L_MSG_MAX_SIZE];
    int ret;

    if (send_mgmt(c, mgmt_id, PTP_MGMT_GET, port) < 0)
        return -1;

    ret = recv_response(c, buf, sizeof(buf));
    if (ret <= 0)
        return -1;

    return parse_response(c, buf, (size_t)ret);
}

int ptp4l_client_get_current_data_set(struct ptp4l_client *c)
{
    c->current_data_valid = 0;
    if (!c->connected)
        return -1;
    return query_tlv(c, PTP_MGMT_CURRENT_DATA_SET, 0);
}

int ptp4l_client_get_port_data_set(struct ptp4l_client *c)
{
    c->port_data_valid = 0;
    if (!c->connected)
        return -1;
    return query_tlv(c, PTP_MGMT_PORT_DATA_SET, 0);
}

int ptp4l_client_get_time_status_np(struct ptp4l_client *c)
{
    c->time_status_valid = 0;
    if (!c->connected)
        return -1;
    return query_tlv(c, PTP_MGMT_TIME_STATUS_NP, 0);
}

int ptp4l_client_get_port_hwclock_np(struct ptp4l_client *c, uint16_t port_number)
{
    c->hwclock_data_valid = 0;
    if (!c->connected)
        return -1;
    return query_tlv(c, PTP_MGMT_PORT_HWCLOCK_NP, port_number);
}

int ptp4l_client_is_readonly_socket(const char *socket_path)
{
    return socket_path && strstr(socket_path, "ptp4lro") != NULL;
}

int ptp4l_client_auto_detect_phc(struct ptp4l_client *c, int32_t *phc_index)
{
    int port_number;

    if (!c || !phc_index || !c->connected)
        return -1;

    *phc_index = -1;
    log_msg(c, "Starting automatic PHC index detection...\n");

    for (port_number = 1; port_number <= 4; port_number++) {
        log_msg(c, "Querying port %d for PHC index...\n", port_number);

        if (ptp4l_client_get_port_hwclock_np(c, (uint16_t)port_number) == 0 &&
            c->hwclock_data_valid && c->hwclock_data.phc_index >= 0) {
            *phc_index = c->hwclock_data.phc_index;
            log_msg(c, "Found PHC index %d on port %d\n", *phc_index, port_number);
            return 0;
        }

        usleep(50000);
    }

    log_msg(c, "Could not auto-detect PHC index via PORT_HWCLOCK_NP\n");
    return -1;
}

int ptp4l_client_query_all(struct ptp4l_client *c)
{
    if (!c->connected)
        return -1;

    if (ptp4l_client_get_current_data_set(c) < 0)
        return -1;
    usleep(50000);

    if (ptp4l_client_get_port_data_set(c) < 0)
        return -1;
    usleep(50000);

    if (ptp4l_client_get_time_status_np(c) < 0)
        return -1;

    c->had_successful_query = 1;
    return 0;
}

void ptp4l_client_init(struct ptp4l_client *c, const char *socket_path,
                       uint8_t domain_number, int verbose)
{
    memset(c, 0, sizeof(*c));
    c->socket_fd = -1;
    c->socket_path = strdup(socket_path);
    c->domain_number = domain_number;
    c->verbose = verbose;
}

void ptp4l_client_cleanup(struct ptp4l_client *c)
{
    ptp4l_client_disconnect(c);
    free(c->socket_path);
    c->socket_path = NULL;
}

int ptp4l_client_connect(struct ptp4l_client *c)
{
    struct sockaddr_un remote, local;
    time_t now = time(NULL);
    char local_path[sizeof(local.sun_path)];

    if (c->connected)
        return 0;

    /*
     * Before the first successful query, keep retrying quickly so a daemon
     * started before ptp4l can attach as soon as the socket appears.
     */
    if (c->had_successful_query &&
        now - c->last_connect_attempt < PTP4L_RECONNECT_INTERVAL)
        return -1;

    if (c->socket_fd >= 0)
        ptp4l_client_disconnect(c);

    c->socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (c->socket_fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    snprintf(local.sun_path, sizeof(local.sun_path), "/tmp/ptp_unc_%d", getpid());
    snprintf(local_path, sizeof(local_path), "%s", local.sun_path);
    unlink(local_path);

    if (bind(c->socket_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind");
        close(c->socket_fd);
        c->socket_fd = -1;
        return -1;
    }

    memset(&remote, 0, sizeof(remote));
    remote.sun_family = AF_UNIX;
    snprintf(remote.sun_path, sizeof(remote.sun_path), "%s", c->socket_path);

    if (connect(c->socket_fd, (struct sockaddr *)&remote, sizeof(remote)) < 0) {
        int err = errno;

        if (err != ENOENT && err != ECONNREFUSED)
            perror("connect");
        else
            log_msg(c, "Waiting for PTP4L socket at %s\n", c->socket_path);

        unlink(local_path);
        close(c->socket_fd);
        c->socket_fd = -1;

        if (c->had_successful_query && err != ENOENT && err != ECONNREFUSED)
            c->last_connect_attempt = now;

        return -1;
    }

    c->connected = 1;
    c->last_connect_attempt = now;
    log_msg(c, "Connected to PTP4L at %s\n", c->socket_path);
    return 0;
}

void ptp4l_client_disconnect(struct ptp4l_client *c)
{
    if (c->socket_fd >= 0) {
        char local_path[256];
        snprintf(local_path, sizeof(local_path), "/tmp/ptp_unc_%d", getpid());
        close(c->socket_fd);
        unlink(local_path);
        c->socket_fd = -1;
    }
    c->connected = 0;
    if (c->had_successful_query)
        c->last_connect_attempt = time(NULL);
}
