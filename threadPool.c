/* 
 * A simple HTTP server with thread pool
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <pthread.h>
#include <syscall.h>
#include <signal.h>

#define BUFSIZE 1024
#define PORTNAME 80
#define POOLSIZE 10
#define RESPONSE_HEADER_SIZE 4096
#define HTTP_PORT 80
#define HTTPS_PORT 443

const char *LOGFILE_PATH = "/var/log/erss-proxy.log";
const char * HTTP = "http";
const char * HTTPS = "https";

typedef struct stack {
  int fd;
  struct stack * next;
} STACK;

typedef struct reqInfo {
  int reqType;
  int isHttps;
  char * host;
} ReqInfo;

STACK * requests;
pthread_t ** threads;
/* 
 * error - wrapper for perror
 */
void error(char *msg) {
  perror(msg);
  exit(0);
}

pthread_mutex_t stackMutex;
pthread_cond_t stackCond;
pthread_mutex_t exitMutex;
pthread_cond_t exitCond;

int running = 1;
int exited = 0;
int serviced = 0;

int listenfd, portno, n;

int stackEmpty() {
  return requests != NULL && requests->fd ? 0 : 1;
}

STACK * pop() {
  if (stackEmpty()) return NULL;
  STACK * tos = requests;
  requests = requests->next;
  return tos;
}

// Only called by main thread whose on main method is not synchronized, therefore needs locking here
void push(int fd) {
  pthread_mutex_lock(&stackMutex);
  STACK * newReq = (STACK*) malloc(sizeof(STACK));
  newReq->fd = fd;
  newReq->next = requests;
  requests = newReq;
  //pthread_cond_broadcast(&stackCond);
  pthread_cond_signal(&stackCond);
  pthread_mutex_unlock(&stackMutex);	
}

void incrExit(int reqsServiced) {
  pthread_mutex_lock(&exitMutex);
  long threadId = syscall(SYS_gettid);
  if (++exited == POOLSIZE) {
    printf("Last thread %ld exiting, about to signal main thread\n", threadId);
    pthread_cond_signal(&exitCond);
  }
  printf("Thread %ld is %d to exit\n", threadId, exited);
  serviced += reqsServiced;
  pthread_mutex_unlock(&exitMutex);
}

void logToFile(char *msg) {
  FILE* f_p;
  f_p = fopen(LOGFILE_PATH, "a+");
  if (f_p == NULL)
    exit(EXIT_FAILURE);
  fprintf(f_p, "%s",msg);
  fclose(f_p);
}

void exitRequest(int connFd, ReqInfo * reqInf, char * request, struct addrinfo * servinfo) {
  close(connFd);
  //free(host);
  free(reqInf);
  free(request);
  if (servinfo != NULL) free(servinfo);
}

ReqInfo * parseRequest(char * stringBuffer) {
  printf("About to parse request of stringBuffer %p\n", stringBuffer);
  printf("Length of request string: %d\n", (int)strlen(stringBuffer));
  if (strlen(stringBuffer) <= 4) {
    puts("Empty request string, returning NULL");
    return NULL;
  }
  puts("Request string:");
  printf("---:\n");
  printf("%s", stringBuffer);
  printf("---:\n");
  char * toBeFreed = stringBuffer;
  int reqType = 0, isHttps = 0, lineNum = 0;
  int isKeepAlive = 0;// TODO
  char * host, * line, * body, * headers;
  while ((line = strsep(&stringBuffer, "\n")) != NULL) {
    lineNum ++;
    printf("Line %d : %s\n", lineNum, line);
    if (lineNum == 1) {
      //split by spaces
      char * word;
      char * originalLine = line;
      while ((word = strsep(&line, " ")) != NULL) {
	if (strcmp(word, "GET") == 0) {
	  puts("GET request detected!");
	  reqType = 0;
	}
	else if (strcmp(word, "CONNECT") == 0) {
	  puts("CONNECT request detected!");
	  reqType = 1;
	  isHttps = 1;
	}
	else if (strcmp(word, "POST") == 0) {
	  puts("POST request detected!");
	  reqType = 2;
	}
	else if (strcmp(word, "HTTP/1.0\r") == 0) {
	  puts("HTTP 1.0 request detected!");
	}
	else if (strcmp(word, "HTTP/1.1\r") == 0) {
	  puts("HTTP 1.1 request detected!");        
	}
	else {
	  printf("Is this the URI? %s\n", word);
	}
	// Reconstruct line
	printf("Reconstructing first line from %s\n", originalLine);
	if (line != NULL) *(originalLine + strlen(originalLine)) = ' ';
	  printf("First line is now %s\n", originalLine);
	}
	puts("Reconstructed first line");
	printf("%s\n", originalLine);
      }
    else { // Different logic for parsing remaining lines as format is Header: <Header Value>
      if (strcmp(line, "\r") == 0) {
	puts("Detected END OF HEADERS!");
	break;
      }
      char * headerName = strsep(&line, ":");
      printf("Header: %s\n", headerName);
      printf("Header value: %s\n", line);
      if (strcmp(headerName, "Host") == 0) { // Logic for parsing hostname
	printf("Parsing host name from %s\n", line);
	char * hostName = strsep(&line, ":"); // Sometimes port is mentioned as well
	char * tempHost = hostName + 1;
	printf("Deduced host name as %s\n", tempHost); // + 1 to account for space after Host:
	host = (char*) malloc((strlen(tempHost) + 1) * sizeof(char));
	strcpy(host, tempHost);
	printf("Restoring host value header from %s\n", hostName);
	if (line != NULL) {
	  printf("Is this port? %s\n", line);
	  *(hostName + strlen(hostName)) = ':';
	}
	else {
	  puts("No port mentioned in host header");
	}
	printf("Restored host value header is %s\n", hostName);
      }
      else {
	printf("Header: %s\n", headerName);
	// TO-DO : Choose important headers, add parsing logic and pass them to forwardRequest method
      }
      printf("Reconstructing header line from %s\n", headerName);
      *(headerName + strlen(headerName)) = ':';
      printf("Reconstructed header: %s\n", headerName);
    }
    printf("Reconstructing headers from %s\n", toBeFreed);
    *(toBeFreed + strlen(toBeFreed)) = '\n';
    printf("Reconstructed headers as %s\n", toBeFreed);
  }
  printf("About to parse body, stringBuffer is %s\n", stringBuffer);
  int emptyBody = 0;
  if (stringBuffer != NULL) {
    printf("String buffer pointer: %p\n", stringBuffer);
    printf("Length of string buffer: %d\n", (int)strlen(stringBuffer));
    body = strsep(&stringBuffer, "\n"); // Now for body
    puts("Completed strsep");
    printf("Body pointer: %p\n", body);
    if (strlen(body) > 0) printf("Body is %s\n", body);
    else {
      puts("Body is empty");
      emptyBody = 1;
    }
  }
  else {
    puts("Body is empty");
    emptyBody = 1;
  }
  *(toBeFreed + strlen(toBeFreed)) = '\n';
  printf("Recovered headers:\n%s", toBeFreed);
  puts("Merging headers and body for forwarding");
  if (!emptyBody) *(toBeFreed + strlen(toBeFreed)) = '\n';            
  ReqInfo * parsedRequest = (ReqInfo *) malloc(sizeof(ReqInfo));
  parsedRequest->reqType = reqType;
  printf("Set reqType to %d\n", reqType);
  parsedRequest->isHttps = isHttps;
  printf("Set isHttps to %d\n", isHttps);
  parsedRequest->host = host;
  printf("Set host to %s\n", host);
  return parsedRequest;
}

void forwardRequest(int connFd, ReqInfo * reqInf, char * request) {
    int reqType = reqInf -> reqType;
    int isHttps = reqInf -> isHttps;
    char * host = reqInf -> host;
    printf("Forwarding request from socket %d, reqType %d, isHttps %d, host %s\n", connFd, reqType, isHttps, host);
    int forwardSock = socket(AF_INET, SOCK_STREAM, 0);
    if (forwardSock < 0) {
      perror("ERROR opening forwarding socket");
      exitRequest(connFd, reqInf, request, NULL);
      return;
    }
    struct addrinfo hints;
    struct addrinfo * servinfo;
    char responseHeaderBuf[RESPONSE_HEADER_SIZE]; // Statically allocate header buffer, then use content-length header to dynamically allocate body buffer
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const char * port = isHttps ? HTTPS : HTTP;
    int lookupResult;
    printf("Port: %s\n", port);
    printf("Address of hints: %p\n", &hints);
    printf("Address of servinfo: %p\n", &servinfo);
    printf("Host for servinfo: %s\n", host);
    if ((lookupResult = getaddrinfo(host, port, &hints, &servinfo))) {
      gai_strerror(lookupResult);
      perror("Unable to getaddrinfo");
      printf("Servinfo: %p\n", servinfo);
      exitRequest(connFd, reqInf, request, servinfo);
      return;
    }
    printf("Successfully obtained server info for %s, lookup result is %d\n", host, lookupResult);
    printf("Servinfo now: %p\n", servinfo);
    printf("Forward sock: %d, ai_addr: %p, addrlen: %d\n", forwardSock, servinfo->ai_addr, (int)(servinfo->ai_addrlen));
    if (connect(forwardSock, servinfo->ai_addr, servinfo->ai_addrlen) < 0) {
      perror("ERROR, unable to connect to host");
      fprintf(stderr, "Unable to connect to host %s\n", host);
      exitRequest(connFd, reqInf, request, servinfo);
      return;
    }

    printf("Successfully connected to %s\n", host);
    int writtenToClient = 0;
    if (reqType == 1) {
      puts("CONNECT request, writing back success to client without writing to origin server");
      char okResponse[94] = "HTTP/1.1 200 Connection established\r\nConnection: Keep-Alive\r\nProxy-Connection: Keep-Alive\r\n\r\n\0";
      // Just for testing
      /*
      memset(request, '\0', BUFSIZE);
      puts("About to try wierd reading without writing back");
      int readBeforeWrite = read(connFd, request, BUFSIZE);
      printf("Bytes read without writing back to client: %d\n", readBeforeWrite);
      */
      writtenToClient = write(connFd, okResponse, strlen(okResponse));
      printf("Bytes written to client for CONNECT ack : %d\n", writtenToClient);
      puts("About to read on CONNECTed socket now");
      // Can overwrite string buffer for original CONNECT request
      memset(request, '\0', BUFSIZE);
      // First-pass : Single-shot read
      printf("Strlen of cleared buffer: %d\n", (int)strlen(request));
      int readAgain = read(connFd, request, BUFSIZE);
      printf("Bytes read again from client: %d\n", readAgain);
      if (readAgain < 0) {
	perror("ERROR reading follow-up request from client after CONNECT");
	exitRequest(connFd, reqInf, request, servinfo);
	return;
      }
      /* MAYBE IT IS ENCRYPTED AND WE CANNOT PARSE IT
      ReqInfo * connectedReqInfo = parseRequest(request);
      if (!connectedReqInfo) {
	puts("Unable to parse request");
	exitRequest(connFd, reqInf, request, servinfo);
	return;
      }
      puts("About to call forward request on the socket's follow-up request");
      free(reqInf);
      free(servinfo);
      forwardRequest(connFd, connectedReqInfo, request); // inefficient : calls getaddrinfo on server whose info is already known
      return;
      */
      int done = 0;
      int writtenToOriginServer;
      while (!done) {
	writtenToOriginServer = write(forwardSock, request, readAgain);
	printf("Relayed encrypted message to origin server, bytes written : %d\n", writtenToOriginServer);
	if (writtenToOriginServer < 0) {
	  perror("ERROR forwarding encrypted response to origin server");
	  exitRequest(connFd, reqInf, request, servinfo);
	  return;
	}
	int readFromOriginServer;
	memset(responseHeaderBuf, '\0', RESPONSE_HEADER_SIZE);
	while ((readFromOriginServer = read(forwardSock, responseHeaderBuf, BUFSIZE)) == BUFSIZE) { // Havent read full data yet
	  printf("Read %d bytes from origin server, about to write to client and then re-read and re-write\n", readFromOriginServer);
	  if ((writtenToClient = write(connFd, responseHeaderBuf, readFromOriginServer)) < 0) {
	    perror("Error forwarding encrypted response from server to client");
	    exitRequest(connFd, reqInf, request, servinfo);
	    return;
	  }
	  printf("Wrote %d bytes to client\n", writtenToClient);
	  memset(responseHeaderBuf, '\0', RESPONSE_HEADER_SIZE);
	}
	printf("Read %d bytes from origin server, done reading for now, writing to client\n", readFromOriginServer);
	if ((writtenToClient = write(connFd, responseHeaderBuf, readFromOriginServer)) < 0) {
	  perror("Error forwarding encrypted response from server to client");
	  //exitRequest(connFd, reqInf, request, servinfo);
	  done = 1;
	  //return;
	}
	printf("Wrote %d bytes to client\n", writtenToClient);
	/* SAFE to exit here? Or must try reading again?
	puts("About to read from client for next forwarding iteration");
	memset(request, '\0', BUFSIZE);
	readAgain = read(connFd, request, BUFSIZE);
	printf("Bytes read again from client: %d\n", readAgain);
	if (readAgain < 0) {
	  perror("ERROR reading follow-up request from client after CONNECT");
	  exitRequest(connFd, reqInf, request, servinfo);
	  return;
	}
	if (readAgain == 0) done = 1;
	*/
	done = 1; // TEMP
      }
      puts("TUNNELING COMPLETE");
      // Maybe this tells client that its over?
      //write(connFd, "\r\n\0", 2);
      exitRequest(connFd, reqInf, request, servinfo);
      return;
    }
    // First-pass : Try to write entire buffer in a single syscall
    int written;
    printf("Length of request: %d\n", (int)strlen(request));
    written = write(forwardSock, request, strlen(request));
    printf("Bytes written to origin server %s: %d\n", host, written);
    //bzero(responseHeaderBuf, RESPONSE_HEADER_SIZE);
    memset(responseHeaderBuf, '\0', RESPONSE_HEADER_SIZE);
    int readFromOrigin = 0;
    int totalReadFromOrigin = 0;
    int totalWrittenToClient = 0;
    
    // Read and write to client in loop
    // RESPONSE_HEADER_SIZE refers to max buffer size
    // Should use content-length header if available
    int done = 0;
    while (!done) {
      while ((readFromOrigin = read(forwardSock, responseHeaderBuf, RESPONSE_HEADER_SIZE)) == RESPONSE_HEADER_SIZE) {
	printf("Read buffer from origin server full at %d bytes, draining to client before next read\n", RESPONSE_HEADER_SIZE);
	writtenToClient = write(connFd, responseHeaderBuf, strlen(responseHeaderBuf));
	totalWrittenToClient += writtenToClient;
	totalReadFromOrigin += readFromOrigin;
	memset(responseHeaderBuf, '\0', RESPONSE_HEADER_SIZE);
      }
      printf("Read fewer bytes than maximum buffer size from origin server : %d\n", readFromOrigin);
      totalReadFromOrigin += readFromOrigin;
      if (readFromOrigin < 0) {
	perror("ERROR reading response from origin server");
	//exitRequest(connFd, host, request, servinfo);
	exitRequest(connFd, reqInf, request, servinfo);
	return;
      }
      if (readFromOrigin == 0) {
	puts("Done reading from server, exiting forwarding loop");
	done = 1;
      }
      else {
	writtenToClient = write(connFd, responseHeaderBuf, strlen(responseHeaderBuf));
	totalWrittenToClient += writtenToClient;
	printf("Total bytes written to client : %d\n", totalWrittenToClient);
	if (totalWrittenToClient < 0 ) {
	  perror("ERROR writing server's response to client");
	  exitRequest(connFd, reqInf, request, servinfo);
	  return;
	}
	memset(responseHeaderBuf, '\0', RESPONSE_HEADER_SIZE);
	
	// TEMP: Try reading to catch SIGPIPE - NVM DOESN'T WORK
	int testClose;
	if ((testClose = write(connFd, "\0", 0)) < 0) {
	  perror("Client socket closed?");
	  done = 1;
	}
	else puts("Re-entering forwarding loop");
      }
      /*
      puts("Testing if server still has bytes to read");
      // TEMP
      memset(responseHeaderBuf, '\0', RESPONSE_HEADER_SIZE);
      readFromOrigin = read(forwardSock, responseHeaderBuf, RESPONSE_HEADER_SIZE);
      printf("Test bytes read: %d\n", readFromOrigin);
      if (readFromOrigin < 0) {
	perror("ERROR reading response from origin server");
	exitRequest(connFd, reqInf, request, servinfo);
      }
      if (readFromOrigin == 0) done = 1;
      else puts("Re-entering forwarding loop");
      */
    }
    printf("Done servicing request, total bytes written from server to client : %d\n", totalWrittenToClient);
    exitRequest(connFd, reqInf, request, servinfo);
}


void * serviceRequest() {
  long threadId = syscall(SYS_gettid);
  printf("Thread %ld here\n", threadId);
  int requestsServiced = 0;
  while(running) {
    pthread_mutex_lock(&stackMutex);
    while(stackEmpty() && running) {
      printf("Thread %ld checking status of stack\n", threadId);
      pthread_cond_wait(&stackCond, &stackMutex);
      printf("Thread %ld returned from wait\n", threadId);
    }
    if (running) {
      STACK * tos = pop();
      int connFd = tos->fd;
      requestsServiced ++;
      printf("Thread %ld servicing request using socket fd %d\n", threadId, connFd);
      free(tos);
      char response[23] = "Booyakasha Bounty!\r\n\r\n\0";
      char *stringBuffer = malloc(BUFSIZE);
      printf("Allocated string buffer pointer %p\n", stringBuffer);
      // Only works if BUFSIZE > size of request, must use while loop
      int rc = read(connFd,stringBuffer,BUFSIZE); 
      if (rc < 0) {
        printf("Thread id %ld unable to read from socket\n", threadId);
      }
      logToFile(stringBuffer);
      char * toBeFreed = stringBuffer; // value of stringBuffer will be modified by strsep, so need to remember pointer to free
      ReqInfo * parsedReq = parseRequest(stringBuffer);
      forwardRequest(connFd, parsedReq, toBeFreed);// Blocking : Opens socket and connection to origin server, writing from that socket to client socket
    }
    pthread_mutex_unlock(&stackMutex);
  }
  printf("Thread %ld exiting after serving %d requests\n", threadId, requestsServiced);
  incrExit(requestsServiced);
}

void spawnThreads() {
  threads = (pthread_t**) malloc(POOLSIZE * sizeof(pthread_t*));
  for (int thread = 0; thread < POOLSIZE; thread ++) {
    *(threads + thread) = (pthread_t*) malloc(sizeof(pthread_t));
    pthread_create(*(threads + thread), NULL, serviceRequest, NULL);
  }
}

void freeAll() {
  STACK * current = requests;
  STACK * next;
  while(current != NULL) {
    next = current->next;
    free(current);
    current = next;
  }
  for (int thread = 0; thread < POOLSIZE; thread ++) {
    free(*(threads + thread));	
  }
  free(threads);
}

void quit() {
  pthread_mutex_lock(&exitMutex);
  pthread_mutex_lock(&stackMutex);
  running = 0;
  pthread_cond_broadcast(&stackCond);
  pthread_mutex_unlock(&stackMutex);
  puts("Main thread waiting for pool threads to exit");
  pthread_cond_wait(&exitCond, &exitMutex);
  printf("Main thread about to exit\n");
  freeAll();
  close(listenfd);
  pthread_mutex_unlock(&exitMutex);
  pthread_mutex_destroy(&stackMutex);
  pthread_mutex_destroy(&exitMutex);
  pthread_cond_destroy(&stackCond);
  pthread_cond_destroy(&exitCond);
  puts("Freed all resources");
  printf("Total requests serviced : %d\n", serviced);
  exit(0);
}

void socketCloseAlert() {
  puts("SOCKET CLOSED");
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
  int userID = getuid();
  printf("User ID: %d\n", userID);
  int effectiveID = geteuid();
  printf("Effective ID: %d\n", effectiveID);
  pthread_mutex_init(&stackMutex, NULL);
  pthread_cond_init(&stackCond, NULL);
  pthread_mutex_init(&exitMutex, NULL);
  printf("Main thread of id %ld about to spawn pool threads\n", syscall(SYS_gettid));
  signal(SIGINT, quit);

  signal(SIGPIPE, socketCloseAlert);
  
  puts("Registered keyboard-interrupt signal-handler");
  spawnThreads();
  struct sockaddr_in serveraddr, cliaddr;
  //char *hostname;
  //char buf[BUFSIZE];

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
  puts("Bind successful, dropping root privilegees");
  seteuid(userID);
  printf("Effective ID : %d\n", geteuid());
  int connfd;
  char response[23] = "Booyakasha Bounty!\r\n\r\n\0";
  socklen_t len = (socklen_t) sizeof(cliaddr);
  listen(listenfd, 10);
  printf("Listening on socket of fd %d\n", listenfd);
  while (running) {
    if ((connfd = accept(listenfd, &cliaddr, &len)) == -1) {
      error("Unable to accept connection");
    }
    printf("Accepted new connection on socket %d\n", connfd);
    push(connfd);
    printf("Pushed connection %d onto stack\n", connfd);
  }

  close(listenfd);
  return 0;
}
