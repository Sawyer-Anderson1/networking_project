
#include "project_502/common.h"
#include "project_502/config_loaders.h"
#include "project_502/md5_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static ClientThreadConfig tracker_cfg;
static ServerThreadConfig upload_cfg;
static char my_ip[INET_ADDRSTRLEN] = "127.0.0.1";

/* ------------------------------------------------------------------ helpers */

static int send_all(int s, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0)
    {
        ssize_t n = send(s, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_full(int s, void *buf, size_t len)
{
    char *p = buf;
    while (len > 0)
    {
        ssize_t n = recv(s, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_line(int s, char *buf, size_t size)
{
    size_t i = 0;
    while (i + 1 < size)
    {
        char c;
        if (recv(s, &c, 1, 0) <= 0)
            return -1;
        if (c == '\n')
        {
            buf[i] = '\0';
            return 0;
        }
        buf[i++] = c;
    }
    return -1;
}

static int connect_to(struct in_addr ip, unsigned short port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = ip;

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(s);
        return -1;
    }
    return s;
}

/* Helper to compute MD5 of a file on disk using compute_md5_hex from md5_handler.c */
static int compute_file_md5_hex(const char *path, char *out_hex)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    if (fsize < 0)
    {
        fclose(f);
        return -1;
    }
    unsigned char *buf = malloc((size_t)fsize + 1);
    if (!buf)
    {
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (n != (size_t)fsize)
    {
        free(buf);
        return -1;
    }
    int rc = compute_md5_hex(buf, (size_t)fsize, out_hex);
    free(buf);
    return rc;
}

/* Connects to tracker, sends one command, and returns the malloc'd reply */
static char *tracker_request(const char *msg)
{
    int s = connect_to(tracker_cfg.server_ip, tracker_cfg.server_port);
    if (s < 0)
    {
        fprintf(stderr, "Cannot connect to tracker %s:%d\n",
                inet_ntoa(tracker_cfg.server_ip), tracker_cfg.server_port);
        return NULL;
    }

    size_t mlen = strlen(msg);
    if (send_all(s, msg, mlen) < 0 ||
        (mlen > 0 && msg[mlen - 1] != '\n' && send_all(s, "\n", 1) < 0))
    {
        fprintf(stderr, "Failed to send request to tracker\n");
        close(s);
        return NULL;
    }

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf)
    {
        close(s);
        return NULL;
    }
    for (;;)
    {
        if (len + 1024 + 1 > cap)
        {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp)
            {
                free(buf);
                close(s);
                return NULL;
            }
            buf = tmp;
        }
        ssize_t n = recv(s, buf + len, cap - len - 1, 0);
        if (n <= 0)
            break;
        len += (size_t)n;
    }
    close(s);
    buf[len] = '\0';
    return buf;
}

static void detect_own_ip(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(tracker_cfg.server_port);
    addr.sin_addr = tracker_cfg.server_ip;
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0)
    {
        struct sockaddr_in me;
        socklen_t len = sizeof(me);
        if (getsockname(s, (struct sockaddr *)&me, &len) == 0)
            inet_ntop(AF_INET, &me.sin_addr, my_ip, sizeof(my_ip));
    }
    close(s);
}

/* ------------------------------------------------------------ upload server */

static void *serve_peer(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char line[512];

    while (read_line(fd, line, sizeof(line)) == 0)
    {
        char *msg = line;
        if (*msg == '<')
            msg++;
        size_t n = strlen(msg);
        while (n > 0 && (msg[n - 1] == '>' || msg[n - 1] == '\r' || msg[n - 1] == ' '))
            msg[--n] = '\0';

        char name[256];
        unsigned long long start, size;
        /* Enforce strict 1024-byte chunk limit */
        if (sscanf(msg, "GET %255s %llu %llu", name, &start, &size) != 3 ||
            size == 0 || size > MAX_CHUNK_SIZE)
        {
            send_all(fd, "<GET invalid>\n", 14);
            continue;
        }

        char path[600];
        snprintf(path, sizeof(path), "%s/%s", upload_cfg.peer_data_folder, name);
        int ffd = open(path, O_RDONLY);
        if (ffd < 0)
        {
            send_all(fd, "<GET ferr>\n", 11);
            continue;
        }

        unsigned char data[MAX_CHUNK_SIZE];
        ssize_t got = pread(ffd, data, (size_t)size, (off_t)start);
        close(ffd);
        if (got <= 0)
        {
            send_all(fd, "<GET invalid>\n", 14);
            continue;
        }

        char reply[64 + MAX_CHUNK_SIZE];
        int hlen = snprintf(reply, 64, "<GET ok %zd>\n", got);
        memcpy(reply + hlen, data, (size_t)got);
        if (send_all(fd, reply, (size_t)hlen + (size_t)got) < 0)
            break;
    }
    close(fd);
    return NULL;
}

static void *upload_server(void *arg)
{
    int listener = (int)(intptr_t)arg;
    for (;;)
    {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0)
            continue;
        pthread_t tid;
        if (pthread_create(&tid, NULL, serve_peer, (void *)(intptr_t)fd) != 0)
        {
            close(fd);
            continue;
        }
        pthread_detach(tid);
    }
    return NULL;
}

static int start_upload_server(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(upload_cfg.peer_port);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(s, SOMAXCONN) < 0)
    {
        perror("bind/listen");
        close(s);
        return -1;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, upload_server, (void *)(intptr_t)s) != 0)
    {
        close(s);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

/* ----------------------------------------------------------- commands */

/* Auto-fills file size, MD5, IP, and port from the local shared directory */
static void cmd_createtracker_auto(const char *filename, const char *desc)
{
    char path[600], md5[MD5_HEX_LEN];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", upload_cfg.peer_data_folder, filename);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
    {
        printf("createtracker: '%s' not found in %s/\n", filename, upload_cfg.peer_data_folder);
        return;
    }
    if (compute_file_md5_hex(path, md5) != 0)
    {
        printf("createtracker: failed to compute MD5 for %s\n", path);
        return;
    }

    char d[256];
    snprintf(d, sizeof(d), "%s", (desc && *desc) ? desc : "shared_file");
    for (char *p = d; *p; p++)
        if (isspace((unsigned char)*p))
            *p = '_';

    char msg[1024];
    snprintf(msg, sizeof(msg), "<createtracker %s %llu %s %s %s %d>\n",
             filename, (unsigned long long)st.st_size, d, md5, my_ip, upload_cfg.peer_port);
    printf("Sending: %s", msg);

    char *reply = tracker_request(msg);
    if (reply)
    {
        printf("%s", reply);
        free(reply);
    }
}

static void cmd_updatetracker_auto(const char *filename, const char *start_s, const char *end_s)
{
    char path[600];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", upload_cfg.peer_data_folder, filename);
    uint64_t start = 0, end = 0;

    if (start_s && end_s)
    {
        start = strtoull(start_s, NULL, 10);
        end = strtoull(end_s, NULL, 10);
    }
    else if (stat(path, &st) == 0)
    {
        end = (uint64_t)st.st_size;
    }

    char msg[1024];
    snprintf(msg, sizeof(msg), "<updatetracker %s %llu %llu %s %d>\n",
             filename, (unsigned long long)start, (unsigned long long)end, my_ip, upload_cfg.peer_port);
    printf("Sending: %s", msg);

    char *reply = tracker_request(msg);
    if (reply)
    {
        printf("%s", reply);
        free(reply);
    }
}

/* Sequential single-peer download for the midterm demo */
static void download_file_from_peer(const char *filename, uint64_t filesize,
                                    const char *expected_md5, const char *peer_ip_str,
                                    unsigned short peer_port)
{
    struct in_addr peer_ip;
    if (inet_pton(AF_INET, peer_ip_str, &peer_ip) != 1)
        return;

    int s = connect_to(peer_ip, peer_port);
    if (s < 0)
    {
        printf("Could not connect to peer %s:%u\n", peer_ip_str, peer_port);
        return;
    }

    char out_path[600];
    snprintf(out_path, sizeof(out_path), "%s/%s", upload_cfg.peer_data_folder, filename);
    FILE *out = fopen(out_path, "wb");
    if (!out)
    {
        perror("fopen");
        close(s);
        return;
    }

    printf("Downloading %s (%llu bytes) from %s:%u...\n",
           filename, (unsigned long long)filesize, peer_ip_str, peer_port);

    uint64_t offset = 0;
    while (offset < filesize)
    {
        uint64_t chunk = filesize - offset;
        if (chunk > MAX_CHUNK_SIZE)
            chunk = MAX_CHUNK_SIZE;

        char req[512], header[64];
        unsigned char data[MAX_CHUNK_SIZE];
        unsigned long got = 0;

        snprintf(req, sizeof(req), "<GET %s %llu %llu>\n",
                 filename, (unsigned long long)offset, (unsigned long long)chunk);

        if (send_all(s, req, strlen(req)) < 0 ||
            read_line(s, header, sizeof(header)) < 0 ||
            sscanf(header, "<GET ok %lu>", &got) != 1 ||
            got != chunk ||
            read_full(s, data, got) < 0)
        {
            printf("Transfer error at byte %llu\n", (unsigned long long)offset);
            fclose(out);
            close(s);
            return;
        }

        fwrite(data, 1, got, out);
        offset += got;
    }

    fclose(out);
    close(s);

    char actual_md5[MD5_HEX_LEN];
    if (compute_file_md5_hex(out_path, actual_md5) == 0 &&
        strncmp(actual_md5, expected_md5, 32) == 0)
    {
        printf("File %s download complete (MD5 verified: %s)\n", filename, actual_md5);
    }
    else
    {
        printf("File %s downloaded, but MD5 mismatch!\n", filename);
    }
}

/* Requests <GET filename.track>, verifies tracker MD5, and downloads the file */
static void cmd_get(const char *trackname)
{
    char msg[512];
    snprintf(msg, sizeof(msg), "<GET %s>\n", trackname);
    char *reply = tracker_request(msg);
    if (!reply)
        return;

    /* Print raw tracker response for the midterm demo */
    printf("%s", reply);

    const char *begin = "<REP GET BEGIN>\n";
    if (strncmp(reply, begin, strlen(begin)) != 0)
    {
        free(reply);
        return;
    }

    char *content = reply + strlen(begin);
    if (*content == '<')
        content++; /* Compat with tracker_commands.c wrapping content in '<...>' */

    char *endtag = strstr(content, "<REP GET END ");
    if (!endtag)
    {
        free(reply);
        return;
    }

    size_t clen = (size_t)(endtag - content);
    /* Strip trailing '>' and '\n' added by tracker_commands.c before <REP GET END */
    if (clen >= 2 && content[clen - 2] == '>' && content[clen - 1] == '\n')
        clen -= 2;
    else if (clen >= 1 && content[clen - 1] == '\n')
        clen -= 1;

    char tracker_md5[MD5_HEX_LEN] = "";
    sscanf(endtag, "<REP GET END %32s>", tracker_md5);

    char got_md5[MD5_HEX_LEN];
    if (compute_md5_hex((unsigned char *)content, clen, got_md5) == 0 &&
        strncmp(got_md5, tracker_md5, 32) == 0)
    {
        printf("[+] Tracker file MD5 verified (%s)\n", got_md5);
    }
    else
    {
        printf("[!] Tracker file MD5 mismatch (got %s, expected %s)\n", got_md5, tracker_md5);
        free(reply); // Release the received response from memory
        return; // Stop before saving or using unverified tracker data
    }

    char tracker_path[600]; // Reserve space for the dest path.
    snprintf(tracker_path, sizeof(tracker_path), "%s/%s", //dest path
            upload_cfg.peer_data_folder, trackname);  // Combine the shared folder and tracker filename.

    FILE *tracker_file = fopen(tracker_path, "wb"); //open file
    if (!tracker_file) //check if opened sucessfully
    {
        perror("Could not save tracker file");
        free(reply); // Release the received response from memory.
        return; // Stop because the tracker file cannot be saved.
    }

    int saved = fwrite(content, 1, clen, tracker_file) == clen; // Check that all bytes were written
    if (fclose(tracker_file) != 0) // Check for errors when closing.
        saved = 0; // Mark the save as failed.

    if (!saved)
    {
        fprintf(stderr, "Failed to write tracker file\n");
        unlink(tracker_path); // Delete the incomplete file.
        free(reply); // Free the response memory.
        return; // Stop here.
    }

printf("Tracker file saved to %s\n", tracker_path);

    /* Parse tracker file for filename, filesize, file MD5, and first available peer */
    char *text = strndup(content, clen);
    free(reply);

    char filename[256] = "", file_md5[MD5_HEX_LEN] = "", peer_ip[32] = "";
    uint64_t filesize = 0;
    unsigned int peer_port = 0;

    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        while (isspace((unsigned char)*line))
            line++;
        if (*line == '\0' || *line == '#')
            continue;

        if (strncmp(line, "Filename:", 9) == 0)
            sscanf(line + 9, " %255s", filename);
        else if (strncmp(line, "Filesize:", 9) == 0)
            filesize = strtoull(line + 9, NULL, 10);
        else if (strncmp(line, "MD5:", 4) == 0)
            sscanf(line + 4, " %32s", file_md5);
        else
        {
            char ip_tmp[32];
            unsigned int port_tmp;
            unsigned long long s_tmp, e_tmp;
            long ts_tmp;
            if (sscanf(line, "%31[0-9.]:%u:%llu:%llu:%ld",
                       ip_tmp, &port_tmp, &s_tmp, &e_tmp, &ts_tmp) == 5)
            {
                /* Pick the first peer that isn't ourselves */
                if (!(strcmp(ip_tmp, my_ip) == 0 && port_tmp == upload_cfg.peer_port))
                {
                    snprintf(peer_ip, sizeof(peer_ip), "%s", ip_tmp);
                    peer_port = port_tmp;
                }
            }
        }
    }
    free(text);

    if (filename[0] && filesize > 0 && peer_ip[0] && peer_port > 0)
    {
        download_file_from_peer(filename, filesize, file_md5, peer_ip, (unsigned short)peer_port);
    }
    else
    {
        printf("[*] Tracker file saved/parsed. (No remote peer found to download %s from)\n",
               filename[0] ? filename : trackname);
    }
}

/* ------------------------------------------------------------------------ main */

static void print_help(void)
{
    printf("\nMidterm Demo Commands:\n"
           "  REQ LIST  (or 'list')                              - request list of tracker files\n"
           "  GET <file>.track                                   - get tracker file & download file\n"
           "  createtracker <file> [desc]                        - auto-create tracker from %s/\n"
           "  createtracker <file> <size> <desc> <md5> <ip> <pt> - manual 6-arg createtracker\n"
           "  updatetracker <file> [start end]                   - auto-update tracker entry\n"
           "  updatetracker <file> <start> <end> <ip> <port>     - manual 5-arg updatetracker\n"
           "  <...>                                              - send raw bracketed command\n"
           "  quit\n\n",
           upload_cfg.peer_data_folder);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    if (!client_thread_config_loader("clientThreadConfig.cfg", &tracker_cfg) ||
        !server_thread_config_loader("serverThreadConfig.cfg", &upload_cfg))
    {
        fprintf(stderr, "Error loading config files.\n");
        return 1;
    }

    mkdir(upload_cfg.peer_data_folder, 0755);
    detect_own_ip();

    if (start_upload_server() != 0)
        return 1;

    printf("Peer listening on %s:%d | Tracker at %s:%d | Shared dir: %s/\n",
           my_ip, upload_cfg.peer_port,
           inet_ntoa(tracker_cfg.server_ip), tracker_cfg.server_port,
           upload_cfg.peer_data_folder);
    print_help();

    char line[1024];
    while (printf("peer> "), fflush(stdout), fgets(line, sizeof(line), stdin) != NULL)
    {
        line[strcspn(line, "\r\n")] = '\0';
        char *cmd = line;
        while (isspace((unsigned char)*cmd))
            cmd++;
        if (*cmd == '\0')
            continue;

        /* 1. Raw bracketed protocol command: <REQ LIST>, <createtracker ...>, etc. */
        if (*cmd == '<')
        {
            char *reply = tracker_request(cmd);
            if (reply)
            {
                printf("%s", reply);
                free(reply);
            }
            continue;
        }

        /* 2. Exact match for "REQ LIST" or "list" */
        if (strcasecmp(cmd, "REQ LIST") == 0 || strcasecmp(cmd, "list") == 0)
        {
            char *reply = tracker_request("<REQ LIST>\n");
            if (reply)
            {
                printf("%s", reply);
                free(reply);
            }
            continue;
        }

        /* Tokenize arguments for GET, createtracker, updatetracker */
        char copy[1024];
        snprintf(copy, sizeof(copy), "%s", cmd);
        char *tok[8] = {0};
        int ntok = 0;
        char *save = NULL;
        for (char *t = strtok_r(copy, " \t", &save); t && ntok < 8; t = strtok_r(NULL, " \t", &save))
            tok[ntok++] = t;

        if (strcasecmp(tok[0], "get") == 0 && ntok >= 2)
        {
            cmd_get(tok[1]);
        }
        else if (strcasecmp(tok[0], "createtracker") == 0)
        {
            if (ntok == 7) /* Manual 6-arg format from rubric */
            {
                char msg[1024];
                snprintf(msg, sizeof(msg), "<%s>\n", cmd);
                char *reply = tracker_request(msg);
                if (reply)
                {
                    printf("%s", reply);
                    free(reply);
                }
            }
            else if (ntok >= 2) /* Auto-fill from local file */
            {
                cmd_createtracker_auto(tok[1], ntok >= 3 ? tok[2] : "shared_file");
            }
            else
                printf("Usage: createtracker <filename> [description]\n");
        }
        else if (strcasecmp(tok[0], "updatetracker") == 0)
        {
            if (ntok == 6) /* Manual 5-arg format from rubric */
            {
                char msg[1024];
                snprintf(msg, sizeof(msg), "<%s>\n", cmd);
                char *reply = tracker_request(msg);
                if (reply)
                {
                    printf("%s", reply);
                    free(reply);
                }
            }
            else if (ntok >= 2) /* Auto-fill IP and port */
            {
                cmd_updatetracker_auto(tok[1], ntok >= 4 ? tok[2] : NULL, ntok >= 4 ? tok[3] : NULL);
            }
            else
                printf("Usage: updatetracker <filename> [start_byte end_byte]\n");
        }
        else if (strcasecmp(tok[0], "help") == 0)
        {
            print_help();
        }
        else if (strcasecmp(tok[0], "quit") == 0 || strcasecmp(tok[0], "exit") == 0)
        {
            break;
        }
        else
        {
            printf("Unknown command. Type 'help'.\n");
        }
    }
    return 0;
}
