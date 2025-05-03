#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>         // for memset
#include <unistd.h>         // for close(), read(), write(), usleep()
#include <arpa/inet.h>      // for inet_pton()
#include <sys/socket.h>     // for socket()
#include <netinet/in.h>     // for sockaddr_in

#define MAXLINE 4096        // Max buffer size
#define TBD 17              // Intended error number
#define SA struct sockaddr  // Stevens-style alias
#define CRUZID_LEN 16       // pad the rest, just to make it more universaL

const char *CRUZID = "zvenzor"; 

struct packet_header {
    int32_t seq_num;
    char cruzid[CRUZID_LEN];
};

void dg_cli(FILE *infile, int sockfd, const SA *pservaddr, socklen_t servlen, int mss)
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
        header.seq_num = htonl(seq);  // network byte order
        memset(header.cruzid, 0, CRUZID_LEN);
        strncpy(header.cruzid, CRUZID, CRUZID_LEN - 1);

        // Copy header into start of buf
        memcpy(buf, &header, sizeof(header));

        ssize_t sent = sendto(sockfd, buf, bytes_read + sizeof(header), 0,
                              pservaddr, servlen);
        if (sent != bytes_read + sizeof(header)) {
            fprintf(stderr, "Packet %d send error or partial send\n", seq);
        } else {
            printf("Sent packet %d (%zu bytes data + %zu header)\n", seq, bytes_read, sizeof(header));
        }

        seq++;
        usleep(500000); // 0.5 sec delay to reduce UDP loss
    }

    free(buf);
}



int main(int argc, char **argv) {
    int sockfd;
    struct sockaddr_in servaddr;

    // ./myclient <server_ip> <server_port> <mss> <in_file_path> <out_file_path>
    if(argc != 6){
        fprintf(stderr, "usage: %s <server_ip> <server_port> <mss> <in_file_path> <out_file_path>\n", argv[0]);
        exit(TBD);
    }

    // Parse command line arguments
    char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    int mss         = atoi(argv[3]);
    char *in_path   = argv[4];
    char *out_path  = argv[5]; // unused for now — will use it in step 2

    // Open input file
    FILE *in_file = fopen(in_path, "rb");
    if (!in_file) {
        perror("input file open error");
        exit(TBD);
    }

    // Set up UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket error");
        exit(EXIT_FAILURE);
    }

    // Set server address
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &servaddr.sin_addr) <= 0) {
        perror("inet_pton error");
        exit(EXIT_FAILURE);
    }

    // Send file in MSS-sized chunks
    dg_cli(in_file, sockfd, (SA *) &servaddr, sizeof(servaddr), mss);

    // Cleanup
    fclose(in_file);
    close(sockfd);
    exit(0);
}
