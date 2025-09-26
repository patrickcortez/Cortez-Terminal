/*
 * net_runner.c
 * Self-contained networking tool for the Cortez Terminal.
 * Implements basic wget, curl, and ftp functionality.
 *
 * Build:
 * gcc -o net-runner net_runner.c
 */

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <ctype.h>
#include <net/if.h>      /* for IFNAMSIZ */
#include <sys/types.h>

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif

#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK 0x8
#endif

#define BUFFER_SIZE 4096

// --- Utility Functions ---

/* Ensure the URL has an http:// prefix. Returns malloc'd string you must free. */
static char *ensure_http_prefix(const char *url_in) {
    if (!url_in) return NULL;
    if (strncasecmp(url_in, "http://", 7) == 0 ||
        strncasecmp(url_in, "https://", 8) == 0) {
        return strdup(url_in);
    }
    size_t n = strlen(url_in) + 8; /* "http://" + null */
    char *out = malloc(n);
    if (!out) return NULL;
    snprintf(out, n, "http://%s", url_in);
    return out;
}

/* Return malloc'd basename from URL path, or "index.html" if none. Caller must free. */
static char *get_basename_from_url(const char *url_in) {
    if (!url_in) return strdup("index.html");
    /* skip scheme if present */
    const char *p = strstr(url_in, "://");
    p = p ? p + 3 : url_in;
    /* find last slash after host */
    const char *last = strrchr(p, '/');
    if (!last) {
        /* no slash -> nothing after host, default to index.html */
        return strdup("index.html");
    }
    const char *name = last + 1;
    if (!*name) return strdup("index.html"); /* ends with / */
    return strdup(name);
}


// Simple helper to print usage and exit
void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <command> [args...]\n", prog);
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  wget <url> [output_file]   - Download a file from a URL.\n");
    fprintf(stderr, "  curl <url>                   - Fetch content from a URL and print to stdout.\n");
    fprintf(stderr, "  ftp get <host> <user> <pass> <remote_path> [local_path] - Download a file via FTP.\n");
    fprintf(stderr, "  ftp list <host> <user> <pass> <remote_path>           - List files in a directory via FTP.\n");
    fprintf(stderr, "  showpeer                     - List local network peers (IP, MAC, hostname).\n");
    exit(EXIT_FAILURE);
}

// Helper to create a TCP connection to a host and port
int connect_to_host(const char *host, int port) {
    int sockfd;
    struct sockaddr_in serv_addr;
    struct hostent *server;

    server = gethostbyname(host);
    if (server == NULL) {
        fprintf(stderr, "ERROR: No such host '%s'\n", host);
        return -1;
    }

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }
    return sockfd;
}

// --- HTTP (wget/curl) Implementation ---

void handle_http(const char *url_str, const char *output_file) {
    if (!url_str) {
        fprintf(stderr, "ERROR: no url\n");
        return;
    }

    /* Work on a local normalized copy so we can parse easily. */
    char *url = ensure_http_prefix(url_str);
    if (!url) {
        fprintf(stderr, "ERROR: out of memory\n");
        return;
    }

    char host[256] = {0};
    char path[1024] = "/";
    int port = 80;

    /* Parse: scheme://host[:port][/path...] */
    char *p = strstr(url, "://");
    char *hostpath = p ? p + 3 : url;
    /* find first slash after host */
    char *slash = strchr(hostpath, '/');
    if (slash) {
        size_t hlen = (size_t)(slash - hostpath);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        strncpy(host, hostpath, hlen);
        host[hlen] = '\0';
        strncpy(path, slash, sizeof(path) - 1);
        path[sizeof(path)-1] = '\0';
    } else {
        strncpy(host, hostpath, sizeof(host) - 1);
        host[sizeof(host)-1] = '\0';
        strcpy(path, "/");
    }

    /* check for optional port in host (host:port) */
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
        if (port <= 0) port = 80;
    }

    /* Connect */
    int sockfd = connect_to_host(host, port);
    if (sockfd < 0) {
        free(url);
        exit(EXIT_FAILURE);
    }

    /* Build and send request. Ensure path begins with '/' */
    if (path[0] != '/') {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "/%s", path);
        strncpy(path, tmp, sizeof(path)-1);
    }

    char request[BUFFER_SIZE];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "User-Agent: net-runner/1.0\r\n\r\n",
             path, host);

    if (write(sockfd, request, strlen(request)) < 0) {
        perror("write");
        close(sockfd);
        free(url);
        exit(EXIT_FAILURE);
    }

    FILE *out_stream = stdout;
    int wrote_to_file = 0;
    if (output_file) {
        out_stream = fopen(output_file, "wb");
        if (!out_stream) {
            perror("fopen");
            close(sockfd);
            free(url);
            exit(EXIT_FAILURE);
        }
        fprintf(stderr, "Downloading to '%s'...\n", output_file);
        wrote_to_file = 1;
    }

    /* Read response and strip headers robustly: accumulate until we find \r\n\r\n */
    char buffer[BUFFER_SIZE];
    ssize_t n;
    size_t header_buf_cap = 8192;
    size_t header_len = 0;
    char *header_buf = malloc(header_buf_cap);
    int headers_done = 0;

    while ((n = read(sockfd, buffer, sizeof(buffer))) > 0) {
        if (!headers_done) {
            /* append to header_buf until we find header terminator */
            if (header_len + (size_t)n >= header_buf_cap) {
                /* expand */
                header_buf_cap *= 2;
                char *tmp = realloc(header_buf, header_buf_cap);
                if (!tmp) {
                    free(header_buf);
                    fprintf(stderr, "Out of memory\n");
                    if (wrote_to_file) fclose(out_stream);
                    close(sockfd);
                    free(url);
                    exit(EXIT_FAILURE);
                }
                header_buf = tmp;
            }
            memcpy(header_buf + header_len, buffer, n);
            header_len += (size_t)n;
            header_buf[header_len] = '\0';

            char *body = strstr(header_buf, "\r\n\r\n");
            if (body) {
                headers_done = 1;
                body += 4; /* skip \r\n\r\n */
                size_t body_off = (size_t)(body - header_buf);
                size_t body_len = header_len - body_off;
                if (body_len > 0) {
                    fwrite(header_buf + body_off, 1, body_len, out_stream);
                }
                /* free header buffer and continue reading rest directly */
                free(header_buf);
                header_buf = NULL;
            }
            /* else keep accumulating */
        } else {
            fwrite(buffer, 1, n, out_stream);
        }
    }

    if (n < 0) perror("read");

    if (wrote_to_file) {
        fprintf(stderr, "Download complete.\n");
        fclose(out_stream);
    }
    if (header_buf) free(header_buf);
    close(sockfd);
    free(url);
}


/* Active LAN peer discovery: non-root TCP connect sweep + reverse DNS + /proc/net/arp MAC lookup */

/* Try a non-blocking TCP connect to ip:port with timeout_ms. Returns 1 on success, 0 otherwise. */
static int try_tcp_connect_timeout(const char *ip_str, int port, int timeout_ms)
{
    int sock = -1, ret = 0;
    struct sockaddr_in sa;
    fd_set wfds;
    struct timeval tv;
    int flags, so_err;
    socklen_t len = sizeof(so_err);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;

    /* set non-blocking */
    flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, ip_str, &sa.sin_addr) != 1) {
        close(sock);
        return 0;
    }

    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        /* immediate success */
        ret = 1;
        goto out;
    }

    if (errno != EINPROGRESS) {
        ret = 0;
        goto out;
    }

    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (select(sock + 1, NULL, &wfds, NULL, &tv) > 0 && FD_ISSET(sock, &wfds)) {
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &len) == 0) {
            if (so_err == 0) ret = 1;
            else ret = 0;
        }
    } else {
        ret = 0; /* timeout or error */
    }

out:
    close(sock);
    return ret;
}

/* Read MAC address for ip from /proc/net/arp. Returns malloc'd string (caller frees) or NULL. */
static char *arp_get_mac(const char *ip)
{
    FILE *f = fopen("/proc/net/arp", "r");
    if (!f) return NULL;
    char line[256];
    /* skip header */
    fgets(line, sizeof(line), f);
    while (fgets(line, sizeof(line), f)) {
        char ipf[64], hw_type[16], flags[16], mac[64], mask[64], device[64];
        if (sscanf(line, "%63s %15s %15s %63s %63s %63s",
                   ipf, hw_type, flags, mac, mask, device) >= 4) {
            if (strcmp(ipf, ip) == 0) {
                fclose(f);
                return strdup(mac);
            }
        }
    }
    fclose(f);
    return NULL;
}

/* Find a primary non-loopback IPv4 + netmask using getifaddrs. Returns 0 on success, -1 on failure.
 * Caller supplies pointers for dotted IP strings (buffers must be at least INET_ADDRSTRLEN bytes).
 */
static int get_local_ipv4_and_mask(char *out_ip, char *out_mask)
{
    struct ifaddrs *ifap, *ifa;
    if (getifaddrs(&ifap) != 0) return -1;

    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
            /* Portable loopback detection: prefer flag, but fall back to name "lo" */
        #if defined(IFF_LOOPBACK)
        if (ifa->ifa_flags & IFF_LOOPBACK) continue; /* skip loopback */
        #else
        if (ifa->ifa_name && strcmp(ifa->ifa_name, "lo") == 0) continue;
        #endif

    struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
    struct sockaddr_in *nm = (struct sockaddr_in *)ifa->ifa_netmask;
        if (!sa || !nm) continue;

        if (inet_ntop(AF_INET, &sa->sin_addr, out_ip, INET_ADDRSTRLEN) == NULL) continue;
        if (inet_ntop(AF_INET, &nm->sin_addr, out_mask, INET_ADDRSTRLEN) == NULL) continue;

        freeifaddrs(ifap);
        return 0;
    }
    freeifaddrs(ifap);
    return -1;
}

/* main active showpeer function */
static void cmd_showpeer_active(void)
{
    char ip_str[INET_ADDRSTRLEN] = {0};
    char mask_str[INET_ADDRSTRLEN] = {0};
    if (get_local_ipv4_and_mask(ip_str, mask_str) != 0) {
        fprintf(stderr, "Could not determine local IPv4 address/netmask (no non-loopback interface found).\n");
        return;
    }

    /* convert dotted strings to uint32 network values */
    struct in_addr ip_addr, mask_addr;
    inet_pton(AF_INET, ip_str, &ip_addr);
    inet_pton(AF_INET, mask_str, &mask_addr);
    uint32_t ip = ntohl(ip_addr.s_addr);
    uint32_t mask = ntohl(mask_addr.s_addr);
    uint32_t network = ip & mask;
    uint32_t broadcast = network | (~mask);

    /* avoid scanning enormous networks; cap to /16 (65534 hosts) or smaller by default */
    uint32_t hosts = (broadcast > network) ? (broadcast - network - 1) : 0;
    if (hosts == 0) {
        fprintf(stderr, "Subnet appears empty.\n");
        return;
    }
    if (hosts > 65534) {
        fprintf(stderr, "Network too large (%u hosts). Aborting.\n", hosts);
        return;
    }

    printf("Local IP: %s  Netmask: %s  Network: %u.%u.%u.%u/%u\n",
           ip_str,
           mask_str,
           (network >> 24) & 0xff, (network >> 16) & 0xff, (network >> 8) & 0xff, network & 0xff,
           32 - __builtin_popcount(mask));

    printf("%-16s %-18s %-24s %s\n", "IP", "MAC", "HOSTNAME", "OPEN_PORTS");
    fflush(stdout);

    const int timeout_ms = 60; /* connect timeout per host in milliseconds; tweak for speed/accuracy */
    const int ports_to_try[] = {80, 22}; /* order: http then ssh (tweakable) */
    const int nports = sizeof(ports_to_try)/sizeof(ports_to_try[0]);

    char try_ip[INET_ADDRSTRLEN];
    for (uint32_t h = network + 1; h < broadcast; ++h) {
        uint32_t iph = h;
        if (iph == ip) continue; /* skip ourself */

        struct in_addr cand;
        cand.s_addr = htonl(iph);
        inet_ntop(AF_INET, &cand, try_ip, sizeof(try_ip));

        int any_open = 0;
        char open_ports[64] = {0};
        for (int pi = 0; pi < nports; ++pi) {
            if (try_tcp_connect_timeout(try_ip, ports_to_try[pi], timeout_ms)) {
                if (any_open) {
                    strcat(open_ports, ",");
                }
                char tmp[16];
                snprintf(tmp, sizeof(tmp), "%d", ports_to_try[pi]);
                strcat(open_ports, tmp);
                any_open = 1;
            }
        }

        if (!any_open) continue; /* skip hosts that didn't respond on ports tried */

        /* reverse lookup */
        char host[NI_MAXHOST] = "(unknown)";
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr = cand;
        if (getnameinfo((struct sockaddr *)&sa, sizeof(sa), host, sizeof(host), NULL, 0, 0) != 0) {
            strncpy(host, "(no-hostname)", sizeof(host)-1);
        }

        char *mac = arp_get_mac(try_ip);
        printf("%-16s %-18s %-24s %s\n", try_ip, mac ? mac : "-", host, open_ports[0] ? open_ports : "-");
        if (mac) free(mac);
        fflush(stdout);
    }
}



// --- FTP Implementation ---

// Reads a single line response from the FTP control connection
int ftp_read_response(int sockfd, char* buffer, size_t size) {
    ssize_t n = read(sockfd, buffer, size - 1);
    if (n > 0) {
        buffer[n] = '\0';
        printf("FTP < %s", buffer); // Log server response
        return atoi(buffer);
    }
    return -1;
}

// Sends a command to the FTP server
void ftp_send_command(int sockfd, const char *command, const char *arg) {
    char buffer[BUFFER_SIZE];
    if (arg) {
        snprintf(buffer, sizeof(buffer), "%s %s\r\n", command, arg);
    } else {
        snprintf(buffer, sizeof(buffer), "%s\r\n", command);
    }
    printf("FTP > %s", buffer);
    write(sockfd, buffer, strlen(buffer));
}

// Enters passive mode and returns a connected data socket
int ftp_enter_pasv(int control_sock) {
    char buffer[BUFFER_SIZE];
    int response_code;

    ftp_send_command(control_sock, "PASV", NULL);
    response_code = ftp_read_response(control_sock, buffer, sizeof(buffer));

    if (response_code != 227) {
        fprintf(stderr, "ERROR: PASV command failed.\n");
        return -1;
    }

    char *start = strchr(buffer, '(');
    if (!start) return -1;
    start++;
    int h1, h2, h3, h4, p1, p2;
    sscanf(start, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2);

    char host[64];
    snprintf(host, sizeof(host), "%d.%d.%d.%d", h1, h2, h3, h4);
    int port = (p1 << 8) + p2;

    printf("FTP Data Connection to %s:%d\n", host, port);
    return connect_to_host(host, port);
}


void handle_ftp(int argc, char **argv) {
    if (argc < 6) usage(argv[0]);
    const char *sub_cmd = argv[2];
    const char *host = argv[3];
    const char *user = argv[4];
    const char *pass = argv[5];
    const char *remote_path = argc > 6 ? argv[6] : "";
    const char *local_path = argc > 7 ? argv[7] : NULL;

    int control_sock = connect_to_host(host, 21);
    if (control_sock < 0) exit(EXIT_FAILURE);

    char buffer[BUFFER_SIZE];
    ftp_read_response(control_sock, buffer, sizeof(buffer)); // 220 Welcome

    ftp_send_command(control_sock, "USER", user);
    ftp_read_response(control_sock, buffer, sizeof(buffer)); // 331 Password required

    ftp_send_command(control_sock, "PASS", pass);
    if (ftp_read_response(control_sock, buffer, sizeof(buffer)) != 230) {
        fprintf(stderr, "ERROR: FTP login failed.\n");
        close(control_sock);
        exit(EXIT_FAILURE);
    }

    if (strcmp(sub_cmd, "list") == 0) {
        int data_sock = ftp_enter_pasv(control_sock);
        if (data_sock < 0) { close(control_sock); exit(EXIT_FAILURE); }

        ftp_send_command(control_sock, "LIST", remote_path);
        ftp_read_response(control_sock, buffer, sizeof(buffer)); // 150

        ssize_t n;
        while ((n = read(data_sock, buffer, BUFFER_SIZE)) > 0) {
            write(STDOUT_FILENO, buffer, n);
        }
        close(data_sock);
        ftp_read_response(control_sock, buffer, sizeof(buffer)); // 226

    } else if (strcmp(sub_cmd, "get") == 0) {
        if (argc < 7) usage(argv[0]);
        
        int data_sock = ftp_enter_pasv(control_sock);
        if (data_sock < 0) { close(control_sock); exit(EXIT_FAILURE); }

        ftp_send_command(control_sock, "TYPE", "I"); // Binary mode
        ftp_read_response(control_sock, buffer, sizeof(buffer)); // 200

        ftp_send_command(control_sock, "RETR", remote_path);
        if (ftp_read_response(control_sock, buffer, sizeof(buffer)) != 150) {
            fprintf(stderr, "ERROR: Could not retrieve file '%s'.\n", remote_path);
            close(data_sock);
            close(control_sock);
            exit(EXIT_FAILURE);
        }

        const char *out_filename = local_path ? local_path : strrchr(remote_path, '/');
        if (out_filename && *out_filename == '/') out_filename++;
        if (!out_filename || !*out_filename) out_filename = "ftp_download";

        FILE* fp = fopen(out_filename, "wb");
        if (!fp) {
            perror("fopen");
            close(data_sock);
            close(control_sock);
            exit(EXIT_FAILURE);
        }
        
        fprintf(stderr, "Downloading '%s' to '%s'...\n", remote_path, out_filename);

        ssize_t n;
        while ((n = read(data_sock, buffer, BUFFER_SIZE)) > 0) {
            fwrite(buffer, 1, n, fp);
        }
        fclose(fp);
        close(data_sock);
        
        fprintf(stderr, "Download complete.\n");
        ftp_read_response(control_sock, buffer, sizeof(buffer)); // 226

    } else {
        usage(argv[0]);
    }

    ftp_send_command(control_sock, "QUIT", NULL);
    ftp_read_response(control_sock, buffer, sizeof(buffer)); // 221
    close(control_sock);
}

static void cmd_showpeer(void)
{
    char host[NI_MAXHOST];
    FILE *f = fopen("/proc/net/arp", "r");
    if (f) {
        char line[512];
        /* skip header line */
        if (!fgets(line, sizeof(line), f)) { fclose(f); return; }

        printf("%-16s %-18s %-8s %s\n", "IP", "MAC", "IFACE", "HOSTNAME");
        while (fgets(line, sizeof(line), f)) {
            char *saveptr = NULL;
            char *ip = strtok_r(line, " \t", &saveptr);
            if (!ip) continue;
            /* skip hw type and flags */
            strtok_r(NULL, " \t", &saveptr);
            strtok_r(NULL, " \t", &saveptr);
            char *mac = strtok_r(NULL, " \t", &saveptr);
            if (!mac) mac = "(none)";
            /* skip mask */
            strtok_r(NULL, " \t", &saveptr);
            char *iface = strtok_r(NULL, " \t\n", &saveptr);
            if (!iface) iface = "(?)";

            /* reverse DNS lookup */
            struct in_addr ina;
            char host[NI_MAXHOST] = "(unknown)";
            if (inet_aton(ip, &ina)) {
                struct sockaddr_in sa;
                memset(&sa, 0, sizeof(sa));
                sa.sin_family = AF_INET;
                sa.sin_addr = ina;
                if (getnameinfo((struct sockaddr *)&sa, sizeof(sa),
                                host, sizeof(host), NULL, 0, 0) != 0) {
                    strncpy(host, "(no-hostname)", sizeof(host)-1);
                    host[sizeof(host)-1] = '\0';
                }
            }

            printf("%-16s %-18s %-8s %s\n", ip, mac, iface, host);
        }
        fclose(f);
        return;
    }

    /* fallback: arp -a (if /proc/net/arp not present) */
    FILE *p = popen("arp -a", "r");
    if (!p) {
        perror("open arp");
        return;
    }
    char buf[512];
    printf("%s\n", "ARP output (fallback):");
    while (fgets(buf, sizeof(buf), p)) {
        fputs(buf, stdout);
    }
    pclose(p);
}



int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
    }

    const char *command = argv[1];

    if (strcmp(command, "wget") == 0) {
    if (argc < 3) usage(argv[0]);
    /* compute default output filename if not provided */
    char *default_name = NULL;
    const char *out_file = NULL;
    if (argc > 3) {
        out_file = argv[3];
    } else {
        default_name = get_basename_from_url(argv[2]);
        out_file = default_name;
    }
    handle_http(argv[2], out_file);
    if (default_name) free(default_name);
} else if (strcmp(command, "curl") == 0) {
    if (argc < 3) usage(argv[0]);
    /* curl prints to stdout */
    handle_http(argv[2], NULL);
} else if (strcmp(command, "ftp") == 0) {
    handle_ftp(argc, argv);
} else if (strcmp(command, "showpeer") == 0) {
    cmd_showpeer_active();
} else {
    fprintf(stderr, "Unknown command: %s\n", command);
    usage(argv[0]);
}


    return EXIT_SUCCESS;
}

