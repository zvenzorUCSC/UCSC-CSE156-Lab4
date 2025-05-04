#include <stdio.h>
#include <stdlib.h>
#include <string.h>         // for memset
#include <unistd.h>         // for close()
#include <arpa/inet.h>      // for htons(), inet_pton()
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>     // for sockaddr_in

#define SERV_PORT 9877      // Default UDP server port
#define MAXLINE 4096        // Max buffer size
#define SA struct sockaddr  // Convenience macro
#define TBD 16              // Arbitrary error code

void dg_echo(int sockfd)
{
    int n;
    char mesg[MAXLINE];

    for ( ; ; ) {
        struct sockaddr_in cliaddr;
        socklen_t len = sizeof(cliaddr);

        n = recvfrom(sockfd, mesg, sizeof(mesg), 0, (SA *)&cliaddr, &len);

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cliaddr.sin_addr, client_ip, INET_ADDRSTRLEN);
        printf("Received %d bytes from %s:%d\n", n, client_ip, ntohs(cliaddr.sin_port));

        if (n < 0) {
            perror("dg_echo recvfrom error");
            continue;
        }

        if (sendto(sockfd, mesg, n, 0, (SA *)&cliaddr, len) != n) {
            perror("dg_echo sendto error");
            exit(TBD);
        }
    }
}

int main(int argc, char **argv)
{
    int sockfd;
    struct sockaddr_in servaddr;
    int port = SERV_PORT;

    // Optional port argument
    if (argc == 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port number: %s\n", argv[1]);
            exit(TBD);
        }
    } else if (argc > 2) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        exit(TBD);
    }

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket error");
        exit(TBD);
    }

    // Zero out server address and bind to any interface + specified port
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if (bind(sockfd, (SA *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind error");
        close(sockfd);
        exit(TBD);
    }

    printf("UDP echo server listening on port %d...\n", port);
    dg_echo(sockfd);

    close(sockfd);
    return 0;
}
