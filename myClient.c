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
#include <libgen.h>     // dirname()
#include <sys/stat.h>   // mkdir()
#include <sys/types.h>
#include <errno.h>


#define MAXLINE 4096
#define TBD 17
#define SA struct sockaddr
#define CRUZID_LEN 16
#define REORDER_BUFFER_SIZE 5

const char *CRUZID = "zvenzor";

struct packet_header {
    int32_t seq_num;
    char cruzid[CRUZID_LEN];
};

struct buffered_packet {
    int seq_num;
    size_t payload_size;
    char *data;
};

int dg_cli(FILE *infile, int sockfd, const SA *pservaddr, socklen_t servlen, int mss)
{
    if (mss <= sizeof(struct packet_header)) {
        fprintf(stderr, "Error: MSS too small for header\n");
        exit(4);
    }

    char *buf = malloc(mss);
    if (!buf) {
        perror("malloc failed");
        exit(TBD);
    }

    size_t data_len = mss - sizeof(struct packet_header);
    char *data_ptr = buf + sizeof(struct packet_header);
    int seq = 0;

    size_t bytes_read;
    while ((bytes_read = fread(data_ptr, 1, data_len, infile)) > 0) {
        struct packet_header header;
        header.seq_num = htonl(seq);
        memset(header.cruzid, 0, CRUZID_LEN);
        strncpy(header.cruzid, CRUZID, CRUZID_LEN - 1);
        memcpy(buf, &header, sizeof(header));

        ssize_t sent = sendto(sockfd, buf, bytes_read + sizeof(header), 0, pservaddr, servlen);
        if (sent != bytes_read + sizeof(header)) {
            fprintf(stderr, "Packet %d send error or partial send\n", seq);
        } else {
            printf("Sent packet %d (%zu bytes data + %zu header)\n", seq, bytes_read, sizeof(header));
        }

        seq++;
        usleep(500000);
    }

    free(buf);
    return seq;
}

void flush_buffered(FILE *outfile, struct buffered_packet *buf, int *count, int *expected_seq, size_t chunk_size)
{
    for (int i = 0; i < *count;) {
        if (buf[i].seq_num == *expected_seq) {
            fseek(outfile, (*expected_seq) * chunk_size, SEEK_SET);
            fwrite(buf[i].data, 1, buf[i].payload_size, outfile);
            printf("Wrote buffered packet %d\n", buf[i].seq_num);
            free(buf[i].data);

            for (int j = i; j < *count - 1; j++) {
                buf[j] = buf[j + 1];
            }
            (*count)--;
            (*expected_seq)++;
        } else {
            i++;
        }
    }
}

void recv_echoed_packets(FILE *outfile, int sockfd, int mss, int expected_packets)
{
    char *recv_buf = malloc(mss);
    if (!recv_buf) {
        perror("malloc failed");
        exit(TBD);
    }

    int *received = calloc(expected_packets, sizeof(int));
    if (!received) {
        perror("calloc failed");
        free(recv_buf);
        exit(TBD);
    }

    struct buffered_packet reorder_buf[REORDER_BUFFER_SIZE];
    int reorder_count = 0;
    int expected_seq = 0;
    int received_count = 0;

    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    size_t header_size = sizeof(struct packet_header);
    size_t data_size = mss - header_size;

    while (received_count < expected_packets) {
        ssize_t n = recvfrom(sockfd, recv_buf, mss, 0, (SA *)&from, &fromlen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "Cannot detect server\n");
                exit(6);
            } else {
                perror("recvfrom error");
                exit(2);
            }
        }

        if ((size_t)n < header_size) {
            fprintf(stderr, "Invalid packet (too small)\n");
            exit(2);
        }

        struct packet_header *hdr = (struct packet_header *)recv_buf;
        int seq_num = ntohl(hdr->seq_num);

        if (seq_num < 0 || seq_num >= expected_packets || received[seq_num]) {
            fprintf(stderr, "Bad or duplicate packet %d\n", seq_num);
            continue;
        }

        size_t payload_size = n - header_size;
        char *payload = malloc(payload_size);
        memcpy(payload, recv_buf + header_size, payload_size);
        received[seq_num] = 1;
        received_count++;

        if (seq_num == expected_seq) {
            fseek(outfile, seq_num * data_size, SEEK_SET);
            fwrite(payload, 1, payload_size, outfile);
            printf("Wrote in-order packet %d\n", seq_num);
            free(payload);
            expected_seq++;
            flush_buffered(outfile, reorder_buf, &reorder_count, &expected_seq, data_size);
        } else {
            if (reorder_count < REORDER_BUFFER_SIZE) {
                reorder_buf[reorder_count].seq_num = seq_num;
                reorder_buf[reorder_count].payload_size = payload_size;
                reorder_buf[reorder_count].data = payload;
                reorder_count++;
                printf("Buffered out-of-order packet %d\n", seq_num);
            } else {
                fprintf(stderr, "Reorder buffer full — dropping packet %d\n", seq_num);
                free(payload);
            }
        }
    }

    free(recv_buf);
    free(received);
}

int create_parent_dirs(const char *filepath) {
    char path_copy[4096];
    strncpy(path_copy, filepath, sizeof(path_copy));
    path_copy[sizeof(path_copy) - 1] = '\0';

    char *dir = dirname(path_copy);
    if (!dir || strcmp(dir, ".") == 0) return 0;  // No directories to make

    char temp[4096] = {0};
    char *token;
    char *rest = dir;

    while ((token = strtok_r(rest, "/", &rest))) {
        strcat(temp, "/");
        strcat(temp, token);

        if (access(temp, F_OK) != 0) {
            if (mkdir(temp, 0755) != 0 && errno != EEXIST) {
                perror("mkdir failed");
                return -1;
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    int sockfd;
    struct sockaddr_in servaddr;

    if (argc != 6) {
        fprintf(stderr, "usage: %s <server_ip> <server_port> <mss> <in_file_path> <out_file_path>\n", argv[0]);
        exit(TBD);
    }

    char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    int mss         = atoi(argv[3]);
    char *in_path   = argv[4];
    char *out_path  = argv[5];

    FILE *in_file = fopen(in_path, "rb");
    if (!in_file) {
        perror("input file open error");
        exit(TBD);
    }

    if (create_parent_dirs(out_path) < 0) {
        fprintf(stderr, "Failed to create directories for output path\n");
        fclose(in_file);
        exit(11);  // Non-standard error code for path creation failure
    }

    FILE *out_file = fopen(out_path, "wb+");
    if (!out_file) {
        perror("output file open error");
        fclose(in_file);
        exit(3);
    }

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket error");
        fclose(in_file);
        fclose(out_file);
        exit(EXIT_FAILURE);
    }

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &servaddr.sin_addr) <= 0) {
        perror("inet_pton error");
        fclose(in_file);
        fclose(out_file);
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    struct timeval timeout;
    timeout.tv_sec = 60;
    timeout.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt failed");
        fclose(in_file);
        fclose(out_file);
        close(sockfd);
        exit(7);
    }

    int total_packets = dg_cli(in_file, sockfd, (SA *)&servaddr, sizeof(servaddr), mss);
    recv_echoed_packets(out_file, sockfd, mss, total_packets);

    fclose(in_file);
    fclose(out_file);
    close(sockfd);
    exit(0);
}
