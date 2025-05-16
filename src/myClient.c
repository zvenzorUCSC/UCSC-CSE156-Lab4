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
#define MAXLINE 4096
#define MAX_MSS 1400
#define SA struct sockaddr

const char *CRUZID = "zvenzor";

struct packet_header {
    int32_t seq_num;
    uint8_t type;      // 0 = INIT, 1 = DATA, 2 = ACK
    uint8_t is_last;
    char cruzid[CRUZID_LEN];
};

void send_init_packet(int sockfd, const struct sockaddr *pservaddr, socklen_t servlen, const char *out_path) {
    struct packet_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = 0;  // INIT
    strncpy(hdr.cruzid, CRUZID, CRUZID_LEN - 1);

    size_t header_size = sizeof(hdr);
    size_t path_len = strlen(out_path) + 1;
    size_t total_len = header_size + path_len;

    char *buf = malloc(total_len);
    if (!buf) {
        perror("malloc failed for INIT");
        exit(10);
    }

    memcpy(buf, &hdr, header_size);
    memcpy(buf + header_size, out_path, path_len);

    if (sendto(sockfd, buf, total_len, 0, pservaddr, servlen) != total_len) {
        perror("sendto INIT failed");
        free(buf);
        exit(11);
    }

    free(buf);
}

int send_data_packets(FILE *infile, int sockfd, const SA *pservaddr, socklen_t servlen, int mss, int *total_packets_out) {
    size_t header_size = sizeof(struct packet_header);
    size_t data_len = mss - header_size;
    char *buf = malloc(mss);
    if (!buf) {
        perror("malloc failed");
        exit(12);
    }

    int seq = 0;
    size_t bytes_read;

    while ((bytes_read = fread(buf + header_size, 1, data_len, infile)) > 0) {
        struct packet_header hdr;
        hdr.seq_num = htonl(seq);
        hdr.type = 1;
        hdr.is_last = 0;
        memset(hdr.cruzid, 0, CRUZID_LEN);
        strncpy(hdr.cruzid, CRUZID, CRUZID_LEN - 1);

        if (feof(infile)) hdr.is_last = 1;

        memcpy(buf, &hdr, header_size);

        if (sendto(sockfd, buf, bytes_read + header_size, 0, pservaddr, servlen) < 0) {
            perror("sendto failed");
            free(buf);
            exit(13);
        }

        printf("Sent packet %d (%zu bytes)\n", seq, bytes_read);
        seq++;
        usleep(100000);
    }

    *total_packets_out = seq;
    free(buf);
    return 0;
}

void recv_acks(int sockfd, int total_packets) {
    int *acked = calloc(total_packets, sizeof(int));
    if (!acked) exit(14);

    int acked_count = 0;
    char buf[128];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    while (acked_count < total_packets) {
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n < sizeof(struct packet_header)) continue;

        struct packet_header *hdr = (struct packet_header *)buf;
        if (hdr->type != 2) continue;

        int seq = ntohl(hdr->seq_num);
        if (seq < 0 || seq >= total_packets || acked[seq]) continue;

        acked[seq] = 1;
        acked_count++;
        printf("Received ACK for packet %d\n", seq);
    }

    free(acked);
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s <server_ip> <port> <mss> <in_file> <out_path>\n", argv[0]);
        exit(1);
    }

    char *server_ip = argv[1];
    int port = atoi(argv[2]);
    int mss = atoi(argv[3]);
    char *in_path = argv[4];
    char *out_path = argv[5];

    if (mss < sizeof(struct packet_header)) {
        fprintf(stderr, "Required minimum MSS is %lu\n", sizeof(struct packet_header));
        exit(1);
    }

    FILE *infile = fopen(in_path, "rb");
    if (!infile) {
        perror("input file error");
        exit(2);
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket error");
        exit(3);
    }

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    inet_pton(AF_INET, server_ip, &servaddr.sin_addr);

    struct timeval timeout = {.tv_sec = 30, .tv_usec = 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    send_init_packet(sockfd, (SA *)&servaddr, sizeof(servaddr), out_path);

    int total_packets = 0;
    send_data_packets(infile, sockfd, (SA *)&servaddr, sizeof(servaddr), mss, &total_packets);
    recv_acks(sockfd, total_packets);

    fclose(infile);
    close(sockfd);
    return 0;
}
