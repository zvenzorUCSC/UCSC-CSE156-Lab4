#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <errno.h>
#include <time.h>

#define CRUZID_LEN 16
#define MAX_MSS 1400
#define TIMEOUT_SEC 2
#define SA struct sockaddr
#define MAX_RETRIES 5

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

// CSV logging helper with RFC 3339 timestamp
void print_csv_log(const char *type, int seq, int base_sn, int next_sn, int winsz) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm *tm_utc = gmtime(&tv.tv_sec);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", tm_utc);
    int millis = tv.tv_usec / 1000;

    printf("%s.%03dZ, %s, %d, %d, %d, %d\n",
           timestamp, millis, type, seq, base_sn, next_sn, base_sn + winsz);
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

int timeval_diff_ms(struct timeval *start, struct timeval *end) {
    return (end->tv_sec - start->tv_sec) * 1000 + (end->tv_usec - start->tv_usec) / 1000;
}

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: %s <server_ip> <server_port> <mss> <winsz> <in_file> <out_path>\n", argv[0]);
        exit(1);
    }

    char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    int mss = atoi(argv[3]);
    int winsz = atoi(argv[4]);
    char *in_path = argv[5];
    char *out_path = argv[6];

    if (mss < sizeof(struct packet_header)) {
        fprintf(stderr, "Required minimum MSS is %lu\n", sizeof(struct packet_header));
        exit(1);
    }

    FILE *infile = fopen(in_path, "rb");
    if (!infile) { perror("fopen input"); exit(2); }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in servaddr;
    socklen_t servlen = sizeof(servaddr);
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &servaddr.sin_addr);

    struct timeval timeout = {.tv_sec = TIMEOUT_SEC, .tv_usec = 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    send_init_packet(sockfd, (SA *)&servaddr, servlen, out_path, mss - sizeof(struct packet_header));

    struct sent_packet *window = calloc(winsz, sizeof(struct sent_packet));
    if (!window) { perror("calloc window"); exit(3); }

    int base_sn = 0, next_sn = 0, eof_reached = 0;
    size_t payload_size = mss - sizeof(struct packet_header);
    time_t last_recv_time = time(NULL);

    while (!eof_reached || base_sn < next_sn) {
        while (next_sn < base_sn + winsz && !eof_reached) {
            char *buf = malloc(mss);
            size_t read = fread(buf + sizeof(struct packet_header), 1, payload_size, infile);

            if (read == 0 && feof(infile)) {
                free(buf);
                eof_reached = 1;
                break;
            }

            int next_byte = fgetc(infile);
            if (next_byte == EOF) {
                ((struct packet_header *)buf)->is_last = 1;
                fprintf(stdout, "Sending final packet: %d\n", next_sn);
                eof_reached = 1;
            } else {
                ((struct packet_header *)buf)->is_last = 0;
                ungetc(next_byte, infile);
            }

            struct packet_header *hdr = (struct packet_header *)buf;
            hdr->seq_num = htonl(next_sn);
            hdr->type = 1;
            strncpy(hdr->cruzid, "zvenzor", CRUZID_LEN - 1);

            int win_idx = next_sn % winsz;
            window[win_idx].seq_num = next_sn;
            window[win_idx].size = read + sizeof(struct packet_header);
            window[win_idx].data = buf;
            window[win_idx].retransmit_count = 0;
            gettimeofday(&window[win_idx].sent_time, NULL);

            sendto(sockfd, buf, window[win_idx].size, 0, (SA *)&servaddr, servlen);
            print_csv_log("DATA", next_sn, base_sn, next_sn + 1, winsz);

            next_sn++;
        }

        struct packet_header ack = {0};
        ssize_t n = recvfrom(sockfd, &ack, sizeof(ack), 0, NULL, NULL);
        if (n >= sizeof(struct packet_header) && ack.type == 2) {
            last_recv_time = time(NULL);
            int ack_sn = ntohl(ack.seq_num);
            if (ack_sn >= base_sn) {
                for (int i = base_sn; i <= ack_sn && i < next_sn; i++) {
                    int win_idx = i % winsz;
                    free(window[win_idx].data);
                }
                base_sn = ack_sn + 1;
                print_csv_log("ACK", ack_sn, base_sn, next_sn, winsz);
            }
        }

        struct timeval now;
        gettimeofday(&now, NULL);

        if (base_sn < next_sn) {
            int win_idx = base_sn % winsz;
            int elapsed = timeval_diff_ms(&window[win_idx].sent_time, &now);

            if (elapsed > TIMEOUT_SEC * 1000) {
                for (int i = base_sn; i < next_sn; i++) {
                    win_idx = i % winsz;

                    window[win_idx].retransmit_count++;
                    fprintf(stderr, "Packet loss detected\n");

                    if (window[win_idx].retransmit_count > MAX_RETRIES) {
                        fprintf(stderr, "Maximum retransmissions exceeded for packet %d\n", window[win_idx].seq_num);
                        exit(4);
                    }

                    sendto(sockfd, window[win_idx].data, window[win_idx].size, 0, (SA *)&servaddr, servlen);
                    gettimeofday(&window[win_idx].sent_time, NULL);
                    print_csv_log("DATA", window[win_idx].seq_num, base_sn, next_sn, winsz);
                }
            }
        }

        if (difftime(time(NULL), last_recv_time) > 30) {
            fprintf(stderr, "Server is down\n");
            exit(5);
        }
    }

    free(window);
    fclose(infile);
    close(sockfd);
    return 0;
}
