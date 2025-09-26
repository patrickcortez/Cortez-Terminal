/*
 * net-twerk.c
 *
 * Small network helper used by Cortez Terminal.
 * Supports:
 *   --ping <ip>
 *   --show
 *   --connect <ssid> [--psk <password>]
 *   --disconnect [--ssid <name>]
 *
 * Behavior:
 *  - Prefers nmcli (NetworkManager) when available for connect/disconnect/show.
 *  - Falls back to iwlist/iw for scanning where possible.
 *  - Uses system 'ping' for ICMP.
 *
 * Build:
 *   gcc -std=c11 -O2 -Wall -Wextra -o tools/net-twerk tools/net-twerk.c
 *
 * This tool is a helper, not a full network manager. Some operations require
 * root or appropriate polkit privileges (especially connect/disconnect).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "Options:\n"
        "  --ping <ip>               Ping an IP (4 pings)\n"
        "  --show                    Show nearby Wi-Fi networks (scan)\n"
        "  --connect <ssid>          Connect to a Wi-Fi network (use --psk to pass password)\n"
        "  --psk <password>          Pre-shared key for --connect\n"
        "  --disconnect              Disconnect / turn Wi-Fi off (nmcli) or require --ssid\n"
        "  --ssid <name>             Specify SSID for disconnect (optional)\n"
        "  --help                    Show this message\n",
        p);
}

/* Return 1 if program exists in PATH, else 0 */
static int program_exists(const char *name) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
    int rc = system(cmd);
    return (rc == 0);
}

/* Run given shell command and stream output to stdout/stderr. Return command exit code. */
static int run_and_stream(const char *cmd) {
    FILE *f = popen(cmd, "r");
    if (!f) {
        fprintf(stderr, "Failed to run command: %s\n", cmd);
        return 127;
    }
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        fputs(buf, stdout);
    }
    int rc = pclose(f);
    if (rc == -1) {
        fprintf(stderr, "pclose failed: %s\n", strerror(errno));
        return 127;
    }
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return 128;
}

/* Escape single quotes for safe embedding inside single-quoted shell arg */
static char *shell_quote_single(const char *s) {
    if (!s) return NULL;
    size_t inlen = strlen(s);
    /* worst case each char becomes 4 chars (' -> '\''), so allocate accordingly */
    size_t outcap = inlen * 4 + 3;
    char *out = malloc(outcap);
    if (!out) return NULL;
    char *p = out;
    *p++ = '\'';
    for (size_t i = 0; i < inlen; ++i) {
        if (s[i] == '\'') {
            memcpy(p, "'\\''", 4);
            p += 4;
        } else {
            *p++ = s[i];
        }
    }
    *p++ = '\'';
    *p = '\0';
    return out;
}

int main(int argc, char **argv) {
    if (argc == 1) {
        usage(argv[0]);
        return 2;
    }

    static struct option longopts[] = {
        {"ping", required_argument, NULL, 1},
        {"show", no_argument, NULL, 2},
        {"connect", required_argument, NULL, 3},
        {"psk", required_argument, NULL, 4},
        {"disconnect", no_argument, NULL, 5},
        {"ssid", required_argument, NULL, 6},
        {"help", no_argument, NULL, 'h'},
        {0,0,0,0}
    };

    char *ping_target = NULL;
    int want_show = 0;
    char *connect_ssid = NULL;
    char *connect_psk = NULL;
    int want_disconnect = 0;
    char *disconnect_ssid = NULL;

    int opt;
    int idx;
    while ((opt = getopt_long(argc, argv, "h", longopts, &idx)) != -1) {
        if (opt == 1) { ping_target = optarg; }
        else if (opt == 2) { want_show = 1; }
        else if (opt == 3) { connect_ssid = optarg; }
        else if (opt == 4) { connect_psk = optarg; }
        else if (opt == 5) { want_disconnect = 1; }
        else if (opt == 6) { disconnect_ssid = optarg; }
        else if (opt == 'h') { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }

    /* Handle ping quickly */
    if (ping_target) {
        char cmd[512];
        /* choose ping command - use ping -c 4 */
        snprintf(cmd, sizeof(cmd), "/bin/ping -c 4 %s", ping_target);
        return run_and_stream(cmd);
    }

    /* Show (scan Wi-Fi) */
    if (want_show) {
        if (program_exists("nmcli")) {
            /* use nmcli for clean table */
            return run_and_stream("nmcli -f SSID,SIGNAL,SECURITY device wifi list");
        } else if (program_exists("iw")) {
            /* try iw dev wlan0 scan (best-effort) */
            /* try to find an interface: pick first wireless by parsing 'iw dev' */
            FILE *f = popen("iw dev | awk '/Interface/ {print $2; exit}'", "r");
            if (!f) {
                fprintf(stderr, "Failed to run iw dev\n");
                return 1;
            }
            char ifname[128] = {0};
            if (fgets(ifname, sizeof(ifname), f) == NULL) {
                pclose(f);
                fprintf(stderr, "No wireless interface found (iw dev).\n");
                return 2;
            }
            pclose(f);
            /* strip newline */
            char *nl = strchr(ifname, '\n'); if (nl) *nl = '\0';
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "iw dev %s scan | awk '/SSID:/{print \"SSID: \" substr($0, index($0,$2))} /signal:/{print \"  \" $0}'", ifname);
            return run_and_stream(cmd);
        } else if (program_exists("iwlist")) {
            /* legacy fallback */
            return run_and_stream("iwlist scanning 2>/dev/null | sed -n 's/.*ESSID:\\(\".*\"\\).*/SSID: \\1/p; s/.*Signal level=\\([0-9\\-]*\\).*/  signal: \\1/p'");
        } else {
            fprintf(stderr, "No suitable wifi scan tool found (nmcli / iw / iwlist). Install NetworkManager (nmcli) for best results.\n");
            return 10;
        }
    }

    /* Connect */
    if (connect_ssid) {
        /* prefer nmcli */
        if (program_exists("nmcli")) {
            char *qssid = shell_quote_single(connect_ssid);
            if (!qssid) { fprintf(stderr, "memory error\n"); return 5; }
            char cmd[1024];
            if (connect_psk) {
                char *qpsk = shell_quote_single(connect_psk);
                if (!qpsk) { free(qssid); fprintf(stderr, "memory error\n"); return 5; }
                /* nmcli will auto-create a connection for SSID and attempt to connect */
                snprintf(cmd, sizeof(cmd), "nmcli device wifi connect %s password %s", qssid, qpsk);
                free(qpsk);
            } else {
                snprintf(cmd, sizeof(cmd), "nmcli device wifi connect %s", qssid);
            }
            free(qssid);
            int rc = run_and_stream(cmd);
            if (rc != 0) {
                fprintf(stderr, "nmcli connect returned %d. You may need root/polkit or to provide --psk.\n", rc);
            }
            return rc;
        } else {
            fprintf(stderr, "No nmcli found. Non-nmcli connect not implemented. Install NetworkManager for this feature.\n");
            return 20;
        }
    }

    /* Disconnect */
    if (want_disconnect) {
        if (program_exists("nmcli")) {
            if (disconnect_ssid) {
                char *qssid = shell_quote_single(disconnect_ssid);
                if (!qssid) { fprintf(stderr, "memory error\n"); return 5; }
                char cmd[512];
                /* try to bring down connection by id (SSID) */
                snprintf(cmd, sizeof(cmd), "nmcli connection down id %s || nmcli connection delete id %s", qssid, qssid);
                free(qssid);
                return run_and_stream(cmd);
            } else {
                /* If no SSID, attempt to turn wifi radio off (global) */
                return run_and_stream("nmcli radio wifi off");
            }
        } else {
            fprintf(stderr, "No nmcli found. Non-nmcli disconnect not implemented. Install NetworkManager for this feature.\n");
            return 21;
        }
    }

    usage(argv[0]);
    return 2;
}
