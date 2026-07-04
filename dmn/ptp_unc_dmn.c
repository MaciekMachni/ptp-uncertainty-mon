/**
 * @file ptp_unc_dmn.c
 * @brief Daemon that polls ptp4l and publishes time uncertainty to shared memory
 */

#include "ptp4l_client.h"
#include "ptp_unc_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PTP_PORT_STATE_SLAVE 9
#define DEFAULT_POLL_MS 1000
#define DEFAULT_MAX_DRIFT_PPB 100000 /* 100 ppm */
#define MAX_CONFIG_LINE 256

static volatile sig_atomic_t keep_running = 1;
static int verbose = 0;
static int poll_interval_ms = DEFAULT_POLL_MS;
static int poll_interval_manual = 0;
static int ptp4l_auto_polling = 0;
static int phc_auto_detect = 0;
static uint8_t ptp4l_domain = 0;
static uint64_t max_drift_ppb = DEFAULT_MAX_DRIFT_PPB;
static char *ptp4l_socket = NULL;
static char *config_path = NULL;
static int32_t known_phc_index = -1;

struct last_valid_sync {
    int      have_sync;
    int64_t  offset_from_master_ns;
    int64_t  mean_path_delay_ns;
    int64_t  master_offset_ns;
    uint64_t ingress_time_ns;
    uint64_t ingress_mono_ns;
    uint32_t port_state;
    uint32_t is_synchronized;
    uint16_t steps_removed;
    uint32_t gm_present;
    uint8_t  gm_id[8];
    int32_t  phc_index;
};

static struct last_valid_sync last_valid;

static void signal_handler(int sig)
{
    (void)sig;
    keep_running = 0;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int64_t time_interval_to_ns(int64_t ti)
{
    return ti >> 16;
}

static uint64_t abs64(int64_t v)
{
    return (uint64_t)(v < 0 ? -v : v);
}

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

static void publish_shm(struct ptp_unc_shm_segment *shm,
                        struct ptp4l_client *client,
                        uint32_t generation)
{
    uint64_t now_mono = monotonic_ns();
    uint64_t total;
    int ptp4l_connected = client->connected ? 1 : 0;
    int32_t phc_index = known_phc_index;

    if (client->connected && client->current_data_valid) {
        last_valid.offset_from_master_ns =
            time_interval_to_ns(client->current_data.offsetFromMaster);
        last_valid.mean_path_delay_ns =
            time_interval_to_ns(client->current_data.meanPathDelay);
        last_valid.steps_removed = client->current_data.stepsRemoved;
        last_valid.have_sync = 1;
    }

    if (client->connected && client->port_data_valid) {
        last_valid.port_state = client->port_data.portState;
        last_valid.is_synchronized =
            (client->port_data.portState == PTP_PORT_STATE_SLAVE) ? 1u : 0u;
    }

    if (client->connected && client->time_status_valid) {
        uint64_t ingress_time_ns = (uint64_t)client->time_status.ingress_time;

        last_valid.master_offset_ns = client->time_status.master_offset;

        if (ingress_time_ns > 0) {
            if (!last_valid.have_sync ||
                ingress_time_ns != last_valid.ingress_time_ns) {
                last_valid.ingress_time_ns = ingress_time_ns;
                last_valid.ingress_mono_ns = now_mono;
            }
            last_valid.have_sync = 1;
        }

        last_valid.gm_present = client->time_status.gmPresent ? 1u : 0u;
        if (client->time_status.gmPresent)
            memcpy(last_valid.gm_id, client->time_status.gmIdentity, 8);
    }

    if (phc_index < 0 && client->hwclock_data_valid)
        phc_index = client->hwclock_data.phc_index;
    if (phc_index >= 0)
        last_valid.phc_index = phc_index;

    total = abs64(last_valid.offset_from_master_ns) +
            abs64(last_valid.mean_path_delay_ns);

    shm->seq = 0;
    __sync_synchronize();

    shm->generation = generation;
    shm->ptp4l_connected = (uint32_t)ptp4l_connected;
    shm->port_state = last_valid.port_state;
    shm->is_synchronized = last_valid.is_synchronized;
    shm->offset_from_master_ns = last_valid.offset_from_master_ns;
    shm->mean_path_delay_ns = last_valid.mean_path_delay_ns;
    shm->steps_removed = last_valid.steps_removed;
    shm->gm_present = last_valid.gm_present;
    memcpy(shm->gm_id, last_valid.gm_id, 8);
    shm->total_uncertainty_ns = total;
    shm->update_timestamp_ns = now_mono;
    shm->master_offset_ns = last_valid.master_offset_ns;
    shm->ingress_time_ns = last_valid.ingress_time_ns;
    shm->ingress_mono_ns = last_valid.ingress_mono_ns;
    shm->max_drift_ppb = max_drift_ppb;
    shm->phc_index = last_valid.phc_index;
    shm->poll_interval_ms = (uint32_t)poll_interval_ms;

    __sync_synchronize();
    shm->seq = generation;
}

static int log_interval_to_poll_ms(int8_t log_interval)
{
    int poll_ms;
    int shift = log_interval;

    /* Bound the shift so the result stays defined and within clamp range. */
    if (shift > 16)
        shift = 16;
    if (shift < -16)
        shift = -16;

    if (shift >= 0)
        poll_ms = (1 << shift) * 500;
    else
        poll_ms = 1000 >> (1 - shift);

    if (poll_ms < 1)
        poll_ms = 1;
    if (poll_ms > 100000)
        poll_ms = 100000;

    return poll_ms;
}

static int update_poll_interval_from_port_data(struct ptp4l_client *client)
{
    int poll_ms;

    if (!ptp4l_auto_polling || poll_interval_manual || !client->port_data_valid)
        return -1;

    poll_ms = log_interval_to_poll_ms(client->port_data.logSyncInterval);
    if (poll_ms < 100)
        poll_ms = 100;
    if (poll_interval_ms == poll_ms)
        return 0;

    poll_interval_ms = poll_ms;
    if (verbose) {
        fprintf(stderr,
                "Auto-detected poll_interval_ms: %d ms from logSyncInterval=%d (2x polling)\n",
                poll_interval_ms, client->port_data.logSyncInterval);
    }
    return 0;
}

static int detect_and_update_phc_index(struct ptp4l_client *client)
{
    int32_t detected = -1;
    int port_number;

    if (!phc_auto_detect || !client->connected)
        return 0;

    for (port_number = 1; port_number <= 4; port_number++) {
        if (ptp4l_client_get_port_hwclock_np(client, (uint16_t)port_number) == 0 &&
            client->hwclock_data_valid && client->hwclock_data.phc_index >= 0) {
            detected = client->hwclock_data.phc_index;
            break;
        }
        usleep(50000);
    }

    if (detected < 0) {
        if (verbose)
            fprintf(stderr, "Could not verify PHC index via PORT_HWCLOCK_NP\n");
        return 0;
    }

    if (known_phc_index >= 0 && detected != known_phc_index) {
        if (verbose) {
            fprintf(stderr, "PHC index change detected: %d -> %d (/dev/ptp%d)\n",
                    known_phc_index, detected, detected);
        }
        known_phc_index = detected;
        return 1;
    }

    if (known_phc_index < 0) {
        known_phc_index = detected;
        if (verbose) {
            fprintf(stderr, "Auto-detected PHC index: %d (/dev/ptp%d)\n",
                    known_phc_index, known_phc_index);
        }
        return 1;
    }

    return 0;
}

static void on_ptp4l_connected(struct ptp4l_client *client)
{
    int readonly = ptp4l_client_is_readonly_socket(client->socket_path);

    if (!readonly) {
        if (ptp4l_client_query_all(client) == 0) {
            update_poll_interval_from_port_data(client);
        }
    } else if (verbose) {
        fprintf(stderr, "Connected to PTP4L read-only socket: %s\n",
                client->socket_path);
    }

    if (phc_auto_detect) {
        if (known_phc_index < 0 &&
            ptp4l_client_auto_detect_phc(client, &known_phc_index) == 0 &&
            verbose) {
            fprintf(stderr, "Auto-detected PHC index: %d (/dev/ptp%d)\n",
                    known_phc_index, known_phc_index);
        } else {
            detect_and_update_phc_index(client);
        }
    }
}

static int query_ptp4l(struct ptp4l_client *client)
{
    int readonly = ptp4l_client_is_readonly_socket(client->socket_path);
    int ret;

    if (readonly) {
        ret = ptp4l_client_get_current_data_set(client);
        if (ret == 0) {
            ptp4l_client_get_port_data_set(client);
            ptp4l_client_get_time_status_np(client);
        }
    } else {
        ret = ptp4l_client_query_all(client);
    }

    if (ret < 0) {
        if (!readonly) {
            if (verbose)
                fprintf(stderr, "ptp4l query failed, disconnecting\n");
            ptp4l_client_disconnect(client);
        } else if (verbose) {
            fprintf(stderr, "ptp4l read-only query failed, keeping connection\n");
        }
        return -1;
    }

    update_poll_interval_from_port_data(client);
    detect_and_update_phc_index(client);
    return client->current_data_valid ? 0 : -1;
}

static int load_config(const char *path)
{
    FILE *f;
    char line[MAX_CONFIG_LINE];
    char key[64], value[256];
    int loaded = 0;

    f = fopen(path, "r");
    if (!f)
        return 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%63s %255s", key, value) != 2)
            continue;

        if (strcmp(key, "ptp4l_socket") == 0) {
            free(ptp4l_socket);
            ptp4l_socket = strdup(value);
            loaded++;
        } else if (strcmp(key, "poll_interval_ms") == 0) {
            poll_interval_ms = atoi(value);
            poll_interval_manual = 1;
            loaded++;
        } else if (strcmp(key, "ptp4l_auto_polling") == 0) {
            ptp4l_auto_polling = atoi(value);
            loaded++;
        } else if (strcmp(key, "phc_auto_detect") == 0) {
            phc_auto_detect = atoi(value);
            loaded++;
        } else if (strcmp(key, "ptp4l_domain") == 0) {
            ptp4l_domain = (uint8_t)atoi(value);
            loaded++;
        } else if (strcmp(key, "verbose") == 0) {
            verbose = atoi(value);
            loaded++;
        } else if (strcmp(key, "max_drift_ppb") == 0) {
            max_drift_ppb = strtoull(value, NULL, 10);
            loaded++;
        }
    }

    fclose(f);
    return loaded;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  -s, --ptp4l-socket PATH   PTP4L management socket (default: /var/run/ptp4l)\n"
            "  -p, --poll-interval MS    Poll interval in milliseconds (default: 1000)\n"
            "  -d, --max-drift-ppb PPB   Max drift bound in ppb (default: 100000 = 100 ppm)\n"
            "  -D, --domain NUM          PTP domain number (default: 0)\n"
            "  -A, --auto-polling        Auto-adjust poll interval from logSyncInterval\n"
            "      --phc-auto-detect     Auto-detect PHC index from ptp4l\n"
            "  -c, --config PATH         Configuration file\n"
            "  -v, --verbose             Verbose logging\n"
            "  -h, --help                Show this help\n",
            prog);
}

int main(int argc, char **argv)
{
    struct ptp4l_client client;
    struct ptp_unc_shm_segment *shm = NULL;
    int shm_fd = -1;
    uint32_t generation = 0;
    int opt;

    static struct option long_opts[] = {
        {"ptp4l-socket", required_argument, 0, 's'},
        {"poll-interval", required_argument, 0, 'p'},
        {"max-drift-ppb", required_argument, 0, 'd'},
        {"domain", required_argument, 0, 'D'},
        {"auto-polling", no_argument, 0, 'A'},
        {"phc-auto-detect", no_argument, 0, 'H'},
        {"config", required_argument, 0, 'c'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    ptp4l_socket = strdup(PTP4L_DEFAULT_SOCKET);
    if (!ptp4l_socket) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    while ((opt = getopt_long(argc, argv, "s:p:d:D:AHc:vh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's':
            free(ptp4l_socket);
            ptp4l_socket = strdup(optarg);
            break;
        case 'p':
            poll_interval_ms = atoi(optarg);
            poll_interval_manual = 1;
            break;
        case 'd':
            max_drift_ppb = strtoull(optarg, NULL, 10);
            break;
        case 'D':
            ptp4l_domain = (uint8_t)atoi(optarg);
            break;
        case 'A':
            ptp4l_auto_polling = 1;
            break;
        case 'H':
            phc_auto_detect = 1;
            break;
        case 'c':
            config_path = optarg;
            break;
        case 'v':
            verbose = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (config_path)
        load_config(config_path);

    if (poll_interval_ms < 100) {
        fprintf(stderr, "poll interval too small, using 100 ms\n");
        poll_interval_ms = 100;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    shm_fd = shm_open(PTP_UNC_SHM_NAME, O_CREAT | O_RDWR, 0644);
    if (shm_fd < 0) {
        perror("shm_open");
        free(ptp4l_socket);
        return 1;
    }

    if (ftruncate(shm_fd, sizeof(*shm)) < 0) {
        perror("ftruncate");
        close(shm_fd);
        free(ptp4l_socket);
        return 1;
    }

    shm = mmap(NULL, sizeof(*shm), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (shm == MAP_FAILED) {
        perror("mmap");
        free(ptp4l_socket);
        return 1;
    }

    memset(shm, 0, sizeof(*shm));
    memset(&last_valid, 0, sizeof(last_valid));
    last_valid.phc_index = -1;
    ptp4l_client_init(&client, ptp4l_socket, ptp4l_domain, verbose);

    if (verbose) {
        fprintf(stderr,
                "ptp_unc_dmn: polling %s every %d ms (max_drift=%lu ppb, domain=%u, auto_polling=%d, phc_auto_detect=%d)\n",
                ptp4l_socket, poll_interval_ms, (unsigned long)max_drift_ppb,
                ptp4l_domain, ptp4l_auto_polling, phc_auto_detect);
    }

    while (keep_running) {
        if (!client.connected) {
            if (ptp4l_client_connect(&client) == 0)
                on_ptp4l_connected(&client);
        }

        if (client.connected) {
            int query_ok = (query_ptp4l(&client) == 0);

            if (query_ok && verbose) {
                fprintf(stderr,
                        "uncertainty: offset=%ld ns delay=%ld ns total=%lu ns state=%u phc=%d\n",
                        (long)last_valid.offset_from_master_ns,
                        (long)last_valid.mean_path_delay_ns,
                        (unsigned long)(abs64(last_valid.offset_from_master_ns) +
                                        abs64(last_valid.mean_path_delay_ns)),
                        last_valid.port_state, last_valid.phc_index);
            } else if (!query_ok && verbose) {
                fprintf(stderr,
                        "ptp4l query failed, keeping last sync (ingress_mono age=%lu ms)\n",
                        last_valid.ingress_mono_ns > 0 ?
                        (unsigned long)((monotonic_ns() - last_valid.ingress_mono_ns) /
                                        1000000ULL) : 0UL);
            }
        }

        if (last_valid.have_sync || (client.connected && client.current_data_valid)) {
            generation++;
            publish_shm(shm, &client, generation);
        }

        sleep_ms(poll_interval_ms);
    }

    if (verbose)
        fprintf(stderr, "shutting down\n");

    munmap(shm, sizeof(*shm));
    shm_unlink(PTP_UNC_SHM_NAME);
    ptp4l_client_cleanup(&client);
    free(ptp4l_socket);
    return 0;
}
