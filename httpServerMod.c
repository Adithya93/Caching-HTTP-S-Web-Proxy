/* 
 * A simple HTTP server
 * usage: httpserv
 */
#include <stdio.h>
#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <pthread.h>
#include <signal.h>
#include <syscall.h>
#include <linux/fs.h>
#include <fcntl.h>

#define BUFSIZE 10000
#define PORTNAME 3000

const char *LOGFILE_PATH = "/var/log/erss-proxy.log";
/* 
 * error - wrapper for perror
 */
void error(char *msg) {
  perror(msg);
  exit(0);
}

void log(char *msg) {
  FILE* f_p;
  f_p = fopen(LOGFILE_PATH, "w+");
  if (f_p == NULL)
    exit(EXIT_FAILURE);
  fprintf(f_p, "%s",msg);
  fclose(f_p);
}

void * serviceRequest(void* connFdHolder) {
  int connFd = *((int*)connFdHolder);
  free(connFdHolder);
  printf("Request served by thread id %ld\n", syscall(SYS_gettid));
  char response[23] = "Booyakasha Bounty!\r\n\r\n\0";
  /*int n = write(connFd, response, strlen(response));*/
  // Do a buffered read of the socket 
  char *stringbuffer = malloc(BUFSIZE);
  int rc;
  rc =read(connFd,stringbuffer,BUFSIZE); 
  /*char *tempbuffer = malloc(BUFSIZE);*/
  /*char *offsetbuffer = stringbuffer; */
  /*int rc;*/
  /*while ((rc = read(connFd, tempbuffer, 1)) > 0) {*/
  /*printf("%s", tempbuffer);*/
  /*strncpy(offsetbuffer, tempbuffer, 1);*/
  /*offsetbuffer = offsetbuffer + (sizeof (char));*/
  /*bzero(tempbuffer, BUFSIZE);*/
  /*}*/
  /*if (rc < 0) {*/
  /*printf("Thread id %ld unable to write to client\n", syscall(SYS_gettid));*/
  /*}*/

  //shutdown(connfd, SHUT_WR);
  log(stringbuffer);
  printf("final result:\n");
  printf("%s", stringbuffer);
  printf("---:\n");
  close(connFd);
}


int main(int argc, char **argv) {
  // Daemonize
  /*pid_t pid;*/
  /*pid = fork();*/
  /*if (pid <0)  */
  /*exit(EXIT_FAILURE);  */
  /*else if (pid > 0)  */
  /*exit(EXIT_SUCCESS); */
  /*if (setsid() == -1)*/
  /*exit(EXIT_FAILURE);  */
  /*signal(SIGCHLD, SIG_IGN);*/
  /*signal(SIGHUP, SIG_IGN);*/
  /*pid = fork();*/
  /*if (pid <0)  */
  /*exit(EXIT_FAILURE);  */
  /*else if (pid > 0)  */
  /*exit(EXIT_SUCCESS); */
  /*umask(0); */
  /*if (chdir("/") == -1)*/
  /*exit(EXIT_FAILURE);  */
  /*for (int i = sysconf(_SC_OPEN_MAX); i >=0 ; i--)*/
  /*close(i);*/
  /*open("/dev/null", O_RDWR); */
  /*dup(0);*/
  /*dup(0);*/
  /*openlog("httpservermod", LOG_PID, LOG_USER);*/
  /*syslog(LOG_INFO, "start logging");*/
  // Now a daemon!

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
    int * fdHolder = (int *) malloc(sizeof(int));
    *fdHolder = connfd;
    pthread_t connThread;
    pthread_create(&connThread, NULL, serviceRequest, (void*)fdHolder);
    /*
       puts("Connection received!");
       n = write(connfd, response, strlen(response));
       if (n < 0) {
       puts("Unable to write to client");
       }
    //shutdown(connfd, SHUT_WR);
    close(connfd);
    */
  }

  close(listenfd);
  return 0;
}
