#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#define MAX_SERVERS 10
#define CRUZID_LEN 16
#define MAX_MSS 1400
#define MAX_RETRIES 5
#define TIMEOUT_SEC 2
#define SA struct sockaddr

struct packet_header {
    int32_t seq_num;
    uint8_t type;
    uint8_t is_last;
    char cruzid[CRUZID_LEN];
};

struct sent_packet {
    int seq_num;
    size_t size;
    char *data;
    int retransmit_count;
    struct timeval sent_time;
};

struct server_info {
    char ip[INET_ADDRSTRLEN];
    int port;
};

struct thread_args {
    struct server_info sinfo;
    const char *in_path;
    const char *out_path;
    int mss;
    int winsz;
    int thread_index;
    int *result_flags;
};

int timeval_diff_ms(struct timeval *start, struct timeval *end) {
    return (end->tv_sec - start->tv_sec) * 1000 + (end->tv_usec - start->tv_usec) / 1000;
}

void log_packet_event(const char *event, int lport, const char *rip, int rport,
                      const char *type, int pktsn, int base, int next, int end) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm = gmtime(&tv.tv_sec);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", tm);
    int millis = tv.tv_usec / 1000;

    printf("%s.%03dZ, %d, %s, %d, %s, %d, %d, %d, %d\n",
           timestamp, millis, lport, rip, rport, type, pktsn, base, next, end);
    fflush(stdout);
}

int load_servers(const char *filename, struct server_info *servers) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("fopen config");
        return -1;
    }
    int count = 0;
    char line[128];
    while (fgets(line, sizeof(line), fp) && count < MAX_SERVERS) {
        if (line[0] == '#') continue;
        if (sscanf(line, "%15s %d", servers[count].ip, &servers[count].port) == 2)
            count++;
    }
    fclose(fp);
    return count;
}

void send_init_packet(int sockfd, const SA *pservaddr, socklen_t servlen, const char *path, int chunk_size) {
    struct packet_header hdr = {0};
    hdr.type = 0;
    strncpy(hdr.cruzid, "zvenzor", CRUZID_LEN - 1);

    int32_t net_chunk_size = htonl(chunk_size);
    size_t header_size = sizeof(hdr);
    size_t path_len = strlen(path) + 1;
    size_t total_len = header_size + path_len + sizeof(int32_t);

    char *buf = malloc(total_len);
    if (!buf) { perror("malloc failed"); exit(1); }

    memcpy(buf, &hdr, header_size);
    memcpy(buf + header_size, path, path_len);
    memcpy(buf + header_size + path_len, &net_chunk_size, sizeof(int32_t));

    sendto(sockfd, buf, total_len, 0, pservaddr, servlen);
    free(buf);
}

void *replicate_to_server(void *arg) {
    struct thread_args *args = (struct thread_args *)arg;
    FILE *infile = fopen(args->in_path, "rb");
    if (!infile) {
        perror("fopen input");
        args->result_flags[args->thread_index] = 1;
        return NULL;
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        args->result_flags[args->thread_index] = 1;
        return NULL;
    }

    struct sockaddr_in servaddr = {0};
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(args->sinfo.port);
    inet_pton(AF_INET, args->sinfo.ip, &servaddr.sin_addr);
    socklen_t servlen = sizeof(servaddr);

    struct sockaddr_in localaddr;
    socklen_t addrlen = sizeof(localaddr);
    getsockname(sockfd, (struct sockaddr *)&localaddr, &addrlen);
    int lport = ntohs(localaddr.sin_port);

    struct timeval timeout = {.tv_sec = TIMEOUT_SEC, .tv_usec = 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    size_t payload_size = args->mss - sizeof(struct packet_header);
    send_init_packet(sockfd, (SA *)&servaddr, servlen, args->out_path, payload_size);

    struct sent_packet *window = calloc(args->winsz, sizeof(struct sent_packet));
    if (!window) { perror("calloc"); exit(1); }

    int base_sn = 0, next_sn = 0, eof_reached = 0;

    while (!eof_reached || base_sn < next_sn) {
        while (next_sn < base_sn + args->winsz && !eof_reached) {
            char *buf = malloc(args->mss);
            size_t read = fread(buf + sizeof(struct packet_header), 1, payload_size, infile);
            if (read == 0 && feof(infile)) {
                free(buf);
                eof_reached = 1;
                break;
            }

            struct packet_header *hdr = (struct packet_header *)buf;
            hdr->seq_num = htonl(next_sn);
            hdr->type = 1;
            hdr->is_last = (read < payload_size || feof(infile)) ? 1 : 0;
            strncpy(hdr->cruzid, "zvenzor", CRUZID_LEN - 1);

            int win_idx = next_sn % args->winsz;
            window[win_idx].seq_num = next_sn;
            window[win_idx].size = read + sizeof(struct packet_header);
            window[win_idx].data = buf;
            window[win_idx].retransmit_count = 0;
            gettimeofday(&window[win_idx].sent_time, NULL);

            sendto(sockfd, buf, window[win_idx].size, 0, (SA *)&servaddr, servlen);
            log_packet_event("SEND", lport, args->sinfo.ip, args->sinfo.port, "DATA",
                             next_sn, base_sn, next_sn + 1, base_sn + args->winsz);
            next_sn++;
        }

        struct packet_header ack = {0};
        ssize_t n = recvfrom(sockfd, &ack, sizeof(ack), 0, NULL, NULL);
        if (n >= sizeof(struct packet_header) && ack.type == 2) {
            int ack_sn = ntohl(ack.seq_num);
            if (ack_sn >= base_sn) {
                for (int i = base_sn; i <= ack_sn && i < next_sn; i++) {
                    int win_idx = i % args->winsz;
                    free(window[win_idx].data);
                }
                base_sn = ack_sn + 1;
                log_packet_event("RECV", lport, args->sinfo.ip, args->sinfo.port, "ACK",
                                 ack_sn, base_sn, next_sn, base_sn + args->winsz);
            }
        }

        struct timeval now;
        gettimeofday(&now, NULL);
        if (base_sn < next_sn) {
            int win_idx = base_sn % args->winsz;
            int elapsed = timeval_diff_ms(&window[win_idx].sent_time, &now);
            if (elapsed > TIMEOUT_SEC * 1000) {
                for (int i = base_sn; i < next_sn; i++) {
                    win_idx = i % args->winsz;
                    window[win_idx].retransmit_count++;
                    fprintf(stderr, "Packet loss detected\n");

                    if (window[win_idx].retransmit_count > MAX_RETRIES) {
                        fprintf(stderr, "Reached max re-transmission limit IP %s\n", args->sinfo.ip);
                        args->result_flags[args->thread_index] = 1;
                        fclose(infile);
                        close(sockfd);
                        return NULL;
                    }

                    sendto(sockfd, window[win_idx].data, window[win_idx].size, 0, (SA *)&servaddr, servlen);
                    gettimeofday(&window[win_idx].sent_time, NULL);
                    log_packet_event("RESEND", lport, args->sinfo.ip, args->sinfo.port, "DATA",
                                     window[win_idx].seq_num, base_sn, next_sn, base_sn + args->winsz);
                }
            }
        }
    }

    free(window);
    fclose(infile);
    close(sockfd);
    args->result_flags[args->thread_index] = 0;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: %s <replication_factor> <serv.conf> <mss> <winsz> <in_file> <out_file_path>\n", argv[0]);
        exit(1);
    }

    int servn = atoi(argv[1]);
    const char *conf = argv[2];
    int mss = atoi(argv[3]);
    int winsz = atoi(argv[4]);
    const char *in_path = argv[5];
    const char *out_path = argv[6];

    if (mss < sizeof(struct packet_header)) {
        fprintf(stderr, "Required minimum MSS is %lu\n", sizeof(struct packet_header));
        exit(1);
    }

    struct server_info servers[MAX_SERVERS];
    int total = load_servers(conf, servers);
    if (total < servn) {
        fprintf(stderr, "Not enough servers in config file\n");
        exit(1);
    }

    pthread_t tids[servn];
    struct thread_args targs[servn];
    int result_flags[servn];
    memset(result_flags, 0, sizeof(result_flags));

    for (int i = 0; i < servn; i++) {
        targs[i] = (struct thread_args){
            .sinfo = servers[i],
            .in_path = in_path,
            .out_path = out_path,
            .mss = mss,
            .winsz = winsz,
            .thread_index = i,
            .result_flags = result_flags
        };
        pthread_create(&tids[i], NULL, replicate_to_server, &targs[i]);
    }

    for (int i = 0; i < servn; i++) {
        pthread_join(tids[i], NULL);
    }

    for (int i = 0; i < servn; i++) {
        if (result_flags[i]) {
            fprintf(stderr, "Replication failed on server %s:%d\n", servers[i].ip, servers[i].port);
            exit(1);
        }
    }

    return 0;
}
