#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#define MAXLINE 32768
#define CRUZID_LEN 16
#define REORDER_BUFFER_SIZE 5
#define SA struct sockaddr

struct packet_header {
    int32_t seq_num;
    uint8_t type;
    uint8_t is_last;
    char cruzid[CRUZID_LEN];
};

struct buffered_packet {
    int seq_num;
    size_t payload_size;
    char *data;
};

int create_parent_dirs(const char *filepath) {
    char path[4096];
    strncpy(path, filepath, sizeof(path));
    path[sizeof(path) - 1] = '\0';

    char *slash = strrchr(path, '/');
    if (!slash) return 0;
    *slash = '\0';

    char current[4096] = {0};
    if (path[0] == '/') strcpy(current, "/");

    char *token = strtok(path, "/");
    while (token) {
        if (strlen(current) + strlen(token) + 2 >= sizeof(current)) return -1;
        if (strlen(current) > 1) strcat(current, "/");
        strcat(current, token);
        if (access(current, F_OK) != 0) {
            if (mkdir(current, 0755) != 0 && errno != EEXIST) return -1;
        }
        token = strtok(NULL, "/");
    }
    return 0;
}

void flush_buffered(FILE *outfile, struct buffered_packet *buf, int *count, int *expected_seq, int chunk_size) {
    for (int i = 0; i < *count;) {
        if (buf[i].seq_num == *expected_seq) {
            fseek(outfile, (*expected_seq) * chunk_size, SEEK_SET);
            fwrite(buf[i].data, 1, buf[i].payload_size, outfile);
            free(buf[i].data);
            for (int j = i; j < *count - 1; j++) buf[j] = buf[j + 1];
            (*count)--;
            (*expected_seq)++;
        } else {
            i++;
        }
    }
}

void print_drop_log(const char *type, int seq_num) {
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", utc);
    printf("%s, DROP %s, %d\n", timestamp, type, seq_num);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> <droppc>\n", argv[0]);
        exit(1);
    }

    srand(time(NULL));  // seed random for drop simulation

    int port = atoi(argv[1]);
    int droppc = atoi(argv[2]);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket error");
        exit(2);
    }

    struct sockaddr_in servaddr = {0}, cliaddr;
    socklen_t len = sizeof(cliaddr);

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if (bind(sockfd, (SA *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind error");
        exit(3);
    }

    char buf[MAXLINE];
    FILE *outfile = NULL;
    int expected_seq = 0;
    int chunk_size = 0;
    struct buffered_packet reorder_buf[REORDER_BUFFER_SIZE];
    int reorder_count = 0;

    while (1) {
        ssize_t n = recvfrom(sockfd, buf, MAXLINE, 0, (SA *)&cliaddr, &len);
        if (n < sizeof(struct packet_header)) continue;

        struct packet_header *hdr = (struct packet_header *)buf;

        if (hdr->type == 0) {
            const char *path = (char *)(buf + sizeof(struct packet_header));
            size_t path_len = strlen(path) + 1;

            if (n < sizeof(struct packet_header) + path_len + sizeof(int32_t)) {
                fprintf(stderr, "INIT packet too short\n");
                continue;
            }

            int32_t net_chunk_size;
            memcpy(&net_chunk_size, buf + sizeof(struct packet_header) + path_len, sizeof(int32_t));
            chunk_size = ntohl(net_chunk_size);

            printf("Received INIT. Saving to: %s\n", path);
            printf("Chunk size: %d\n", chunk_size);

            if (create_parent_dirs(path) < 0) {
                fprintf(stderr, "Could not create directory\n");
                exit(4);
            }

            outfile = fopen(path, "wb");
            if (!outfile) {
                perror("fopen failed");
                exit(5);
            }

            expected_seq = 0;
            reorder_count = 0;
            continue;
        }

        if (hdr->type != 1 || outfile == NULL) continue;

        int seq_num = ntohl(hdr->seq_num);

        // Simulate drop of incoming DATA packet
        if ((rand() % 100) < droppc) {
            print_drop_log("DATA", seq_num);
            continue;
        }

        size_t payload_size = n - sizeof(struct packet_header);
        char *payload = malloc(payload_size);
        memcpy(payload, buf + sizeof(struct packet_header), payload_size);

        if (seq_num == expected_seq) {
            fseek(outfile, seq_num * chunk_size, SEEK_SET);
            fwrite(payload, 1, payload_size, outfile);
            free(payload);
            expected_seq++;
            flush_buffered(outfile, reorder_buf, &reorder_count, &expected_seq, chunk_size);
        } else {
            if (reorder_count < REORDER_BUFFER_SIZE) {
                reorder_buf[reorder_count].seq_num = seq_num;
                reorder_buf[reorder_count].payload_size = payload_size;
                reorder_buf[reorder_count].data = payload;
                reorder_count++;
            } else {
                fprintf(stderr, "Reorder buffer full — dropping packet %d\n", seq_num);
                free(payload);
            }
        }

        if (hdr->is_last) {
            printf("Last packet received (flagged by client).\n");
            fflush(outfile);
            int fd = fileno(outfile);
            long final_size = (seq_num * chunk_size) + payload_size;
            if (ftruncate(fd, final_size) != 0) {
                perror("ftruncate failed");
            } else {
                printf("File truncated to final size: %ld bytes\n", final_size);
            }
        }

        struct packet_header ack = {0};
        ack.seq_num = htonl(expected_seq - 1);
        ack.type = 2;
        strncpy(ack.cruzid, hdr->cruzid, CRUZID_LEN);
        
        // Simulate drop of outgoing ACK packet
        if ((rand() % 100) < droppc) {
            print_drop_log("ACK", expected_seq - 1);
        } else {
            sendto(sockfd, &ack, sizeof(ack), 0, (SA *)&cliaddr, len);
            printf("ACK sent for seq %d (cumulative)\n", expected_seq - 1);
        }
    }

    if (outfile) fclose(outfile);
    close(sockfd);
    return 0;
}
