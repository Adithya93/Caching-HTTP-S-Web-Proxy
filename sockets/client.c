#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <arpa/inet.h>
#include <errno.h>

int MAXLINE = 4096;

int main(int argc, char **argv) {
	int     sockfd, n;
	char    recvline[MAXLINE + 1];
	struct sockaddr_in servaddr;
	if (argc != 2) {
		perror("usage: a.out <IPaddress>");
		exit(1);
	}
	if ( (sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket error");
		exit(1);
	}	
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(13);  /* daytime server */
	if (inet_pton(AF_INET, argv[1], &servaddr.sin_addr) <= 0) { 
		fprintf(stderr, "inet_pton error for %s", argv[1]);
		exit(1);
	}
	printf("Connecting to ip address %ul\n", servaddr.sin_addr.s_addr);
	if (connect(sockfd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0) {
		perror("connect error");
		exit(1);
	}
	while ( (n = read(sockfd, recvline, MAXLINE)) > 0) {
    		recvline[n] = 0;        /* null terminate */
    		if (fputs(recvline, stdout) == EOF) { 
			perror("fputs error");
			exit(1);
		}
	}
	if (n < 0) {
		perror("read error");
		exit(1);
	}
	puts("Server exiting.");	
	exit(0);
}
