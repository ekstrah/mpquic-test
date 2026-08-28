#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

typedef struct st_mp_config_t {
    int is_server;
    int port;
    char server_ip[64];
    char path_spec[256];   /* raw -A value, parsed further in Task 3 */
    char cc_algo[32];
    int duration_sec;
} mp_config_t;

static void mp_usage(const char* prog) {
    fprintf(stderr, "Server: %s -p <port> [-G <cc_algo>]\n", prog);
    fprintf(stderr, "Client: %s -A <path_spec> [-G <cc_algo>] --duration <seconds> <server_ip> <port>\n", prog);
}

/* Returns 0 on success, -1 on bad arguments (mp_usage already printed). */
int mp_parse_args(int argc, char** argv, mp_config_t* config) {
    memset(config, 0, sizeof(mp_config_t));
    strncpy(config->cc_algo, "cubic", sizeof(config->cc_algo) - 1);
    config->duration_sec = 30;

    int opt;
    /* getopt_long needed for --duration; picoquic's own getopt.c (used by
       picoquicdemo) is short-option-only per Phase 0 research, so this
       app uses the system getopt_long instead. */
    static struct option long_opts[] = {
        {"duration", required_argument, 0, 'd'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "p:A:G:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p':
            config->port = atoi(optarg);
            config->is_server = 1;
            break;
        case 'A':
            strncpy(config->path_spec, optarg, sizeof(config->path_spec) - 1);
            break;
        case 'G':
            strncpy(config->cc_algo, optarg, sizeof(config->cc_algo) - 1);
            break;
        case 'd':
            config->duration_sec = atoi(optarg);
            break;
        default:
            mp_usage(argv[0]);
            return -1;
        }
    }

    if (!config->is_server) {
        /* Client mode: expect <server_ip> <port> as positional args. */
        if (optind + 2 > argc) {
            fprintf(stderr, "Client mode requires server_ip and port.\n");
            mp_usage(argv[0]);
            return -1;
        }
        strncpy(config->server_ip, argv[optind], sizeof(config->server_ip) - 1);
        config->port = atoi(argv[optind + 1]);
    }

    return 0;
}

int main(int argc, char** argv) {
    mp_config_t config;
    if (mp_parse_args(argc, argv, &config) != 0) {
        return 1;
    }

    if (config.is_server) {
        printf("server mode: port=%d cc=%s\n", config.port, config.cc_algo);
    } else {
        printf("client mode: server=%s port=%d path_spec=%s cc=%s duration=%d\n",
            config.server_ip, config.port, config.path_spec, config.cc_algo, config.duration_sec);
    }
    return 0;
}
