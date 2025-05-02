#include <stdio.h>
#include <stdlib.h>
#include <string.h>         // for memset
#include <unistd.h>         // for close(), read(), write()
#include <arpa/inet.h>      // for inet_pton()
#include <sys/socket.h>     // for socket()
#include <netinet/in.h>     // for sockaddr_in

#define SERV_PORT 9877      // Match with server
#define MAXLINE 4096        // For buffers

// Define SA as Stevens does (optional)
#define SA struct sockaddr

void dg_cli(FILE *fp, int sockfd, const SA *pservaddr, socklen_t servlen)
{
    int n;
    char sendline[MAXLINE], recvline[MAXLINE + 1];

    while (fgets(sendline, MAXLINE, fp) != NULL) {
        if (sendto(sockfd, sendline, strlen(sendline), 0, pservaddr, servlen) < 0) {
            perror("sendto error");
            continue;
        }

        if ((n = recvfrom(sockfd, recvline, MAXLINE, 0, NULL, NULL)) < 0) {
            perror("recvfrom error");
            continue;
        }

        recvline[n] = 0; // null terminate
        if (fputs(recvline, stdout) == EOF) {
            perror("fputs error");
            break;
        }
    }
}

int main(int argc, char **argv)
{
    int sockfd;
    struct sockaddr_in servaddr;

    if(argc != 2)
        perror("usage: udpcli <IPaddress>");

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(SERV_PORT);

    if (inet_pton(AF_INET, argv[1], &servaddr.sin_addr) <= 0) {
        perror("inet_pton error");
        exit(EXIT_FAILURE);
    }

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket error");
        exit(EXIT_FAILURE); //defined in stdlib as 1
    }

    dg_cli(stdin, sockfd, (SA *) &servaddr, sizeof(servaddr));

    exit(0);
}