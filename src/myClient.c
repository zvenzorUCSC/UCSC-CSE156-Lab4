// myClient.c with Go-Back-N sliding window implementation (with final packet fix and debug message)

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

#define CRUZID_LEN 16
#define MAX_MSS 1400
#define MAX_PACKETS 32768
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

void send_init_packet(int sockfd, const SA *pservaddr, socklen_t servlen, const char *path) {
    struct packet_header hdr = {0};
    hdr.type = 0;
    strncpy(hdr.cruzid, "zvenzor", CRUZID_LEN - 1);

    size_t header_size = sizeof(hdr);
    size_t path_len = strlen(path) + 1;
    size_t total_len = header_size + path_len;

    char *buf = malloc(total_len);
    memcpy(buf, &hdr, header_size);
    memcpy(buf + header_size, path, path_len);

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

    send_init_packet(sockfd, (SA *)&servaddr, servlen, out_path);

    struct sent_packet window[MAX_PACKETS] = {0};
    int base_sn = 0, next_sn = 0, total_packets = 0;
    size_t payload_size = mss - sizeof(struct packet_header);

    // Read entire file into packet buffer (fix: only create packets with data)
    while (1) {
        char *buf = malloc(mss);
        size_t read = fread(buf + sizeof(struct packet_header), 1, payload_size, infile);
        if (read == 0) {
            free(buf);
            break;
        }

        struct packet_header *hdr = (struct packet_header *)buf;
        hdr->seq_num = htonl(total_packets);
        hdr->type = 1;
        hdr->is_last = feof(infile);
        strncpy(hdr->cruzid, "zvenzor", CRUZID_LEN - 1);

        if (hdr->is_last) {
            printf("Sending final packet: %d\n", total_packets);
        }

        window[total_packets].seq_num = total_packets;
        window[total_packets].size = read + sizeof(struct packet_header);
        window[total_packets].data = buf;
        window[total_packets].retransmit_count = 0;

        total_packets++;
    }

    while (base_sn < total_packets) {
        // Send packets within window
        while (next_sn < base_sn + winsz && next_sn < total_packets) {
            sendto(sockfd, window[next_sn].data, window[next_sn].size, 0, (SA *)&servaddr, servlen);
            gettimeofday(&window[next_sn].sent_time, NULL);
            printf("Sent DATA, %d\n", next_sn);
            next_sn++;
        }

        // Receive ACKs
        struct packet_header ack;
        ssize_t n = recvfrom(sockfd, &ack, sizeof(ack), 0, NULL, NULL);
        if (n >= sizeof(struct packet_header) && ack.type == 2) {
            int ack_sn = ntohl(ack.seq_num);
            if (ack_sn >= base_sn) {
                base_sn = ack_sn + 1;
            }
        }

        // Timeout check
        struct timeval now;
        gettimeofday(&now, NULL);
        if (base_sn < next_sn) {
            int elapsed = timeval_diff_ms(&window[base_sn].sent_time, &now);
            if (elapsed > TIMEOUT_SEC * 1000) {
                printf("Timeout — resending window\n");
                for (int i = base_sn; i < next_sn; i++) {
                    sendto(sockfd, window[i].data, window[i].size, 0, (SA *)&servaddr, servlen);
                    gettimeofday(&window[i].sent_time, NULL);
                    printf("Resent DATA, %d\n", i);
                }
            }
        }
    }

    for (int i = 0; i < total_packets; i++) free(window[i].data);
    fclose(infile);
    close(sockfd);
    return 0;
}
