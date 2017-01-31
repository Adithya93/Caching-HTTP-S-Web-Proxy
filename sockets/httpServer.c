/* 
 * A simple HTTP server
 * usage: httpserv
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 

#define BUFSIZE 1024
#define PORTNAME 3000
/* 
 * error - wrapper for perror
 */
void error(char *msg) {
    perror(msg);
    exit(0);
}

int main(int argc, char **argv) {
    int listenfd, portno, n;
    struct sockaddr_in serveraddr, cliaddr;
    char *hostname;
    char buf[BUFSIZE];

    socklen_t socklen;

    portno = PORTNAME;
    /* socket: create the socket */
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) 
        error("ERROR opening listening socket");

    /* build the server's Internet address */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons(portno);
    if(bind(listenfd, (struct sockaddr_in *) &serveraddr, (socklen_t)sizeof(serveraddr))) {
	error("Unable to bind");
    }
 
    int connfd;
    char response[23] = "Booyakasha Bounty!\r\n\r\n\0";
    socklen_t len = (socklen_t) sizeof(cliaddr);
    listen(listenfd, 10);
    printf("Listening on socket of fd %d\n", listenfd);
    while (1) {
	if ((connfd = accept(listenfd, &cliaddr, &len)) == -1) {
		error("Unable to accept connection");
	}
	puts("Connection received!");
	n = write(connfd, response, strlen(response));
	if (n < 0) {
		puts("Unable to write to client");
	}
	//shutdown(connfd, SHUT_WR);
	close(connfd);
    }
    
    close(listenfd);
    return 0;
}
