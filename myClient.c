#include <stdio.h>
#include <stdlib.h>
#include <string.h>         // for memset
#include <unistd.h>         // for close(), read(), write(), usleep()
#include <arpa/inet.h>      // for inet_pton()
#include <sys/socket.h>     // for socket()
#include <netinet/in.h>     // for sockaddr_in

#define MAXLINE 4096        // Max buffer size
#define TBD 17              // Intended error number
#define SA struct sockaddr  // Stevens-style alias

void dg_cli(FILE *infile, int sockfd, const SA *pservaddr, socklen_t servlen, int mss)
{
    char *buf = malloc(mss);
    if (!buf) {
        perror("malloc failed");
        exit(TBD);
    }

    int seq = 0;
    size_t bytes_read;

    while ((bytes_read = fread(buf, 1, mss, infile)) > 0) {
        ssize_t sent = sendto(sockfd, buf, bytes_read, 0, pservaddr, servlen);
        if (sent != bytes_read) {
            fprintf(stderr, "Packet %d: sendto error or incomplete send\n", seq);
            // Optional: handle retry here
        } else {
            printf("Sent packet %d (%zu bytes)\n", seq, bytes_read);
        }

        seq++;
        usleep(500000); // 0.5 second delay to prevent buffer overflows, as per suggestion in lab.
    }

    free(buf);
}


int main(int argc, char **argv)
{
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
    memset(&servaddr, 0, sizeof(servaddr));
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
