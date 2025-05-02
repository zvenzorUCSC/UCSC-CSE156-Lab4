#include <stdio.h>
#include <stdlib.h>
#include <string.h>         // for memset
#include <unistd.h>         // for close()
#include <arpa/inet.h>      // for htons(), htonl(), inet_pton()
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>     // for sockaddr_in

#define SERV_PORT 9877      // Default UDP server port
#define MAXLINE 4096        // Max buffer size
#define SA struct sockaddr  // This is what the textbook does

void dg_echo(int sockfd, SA *pcliaddr, socklen_t clilen)
{
    int n;
    socklen_t len;
    char mesg[MAXLINE];

    for ( ; ; ) {
        len = clilen;

        if ((n = recvfrom(sockfd, mesg, sizeof(mesg), 0, pcliaddr, &len)) < 0){
            perror("dg_echo recvfrom error");
            continue;
        }

        if (sendto(sockfd, mesg, n, 0, pcliaddr, len) != n)
            perror("dg_echo sendto error");

    }
}


int main(int argc, char **argv) {
    int sockfd;
    struct sockaddr_in servaddr, cliaddr;

    // create the socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        perror("socket error");

    // zero out the server address
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(SERV_PORT);

    // bind the socket to the address
    if (bind(sockfd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        perror("bind error");
    
    // Echo server loop from unp.h in the textbook
    dg_echo(sockfd, (SA *) &cliaddr, sizeof(cliaddr));

    close(sockfd);
    return 0;
}