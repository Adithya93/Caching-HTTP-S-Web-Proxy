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
#include "./cache.c"

#define BUFSIZE 1024
#define PORTNAME 80
#define POOLSIZE 10
#define RESPONSE_HEADER_SIZE 8192
#define HTTP_PORT 80
#define HTTPS_PORT 443
#define PRINTABLE_ASCII 32
int DEBUG = 1;

const char *LOGFILE_PATH = "/var/log/erss-proxy.log";
const char *HTTP = "http";
const char *HTTPS = "https";
const char *BAD_REQUEST_RESPONSE = "HTTP/1.1 400 Bad Request\r\n\0";


typedef struct stack {
  int fd;
  struct stack * next;
} STACK;

typedef struct reqInfo {
  int reqType;
  int isHttps;
  char *host;
  char *URI;
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
  return (requests != NULL && requests->fd) ? 0 : 1;
}

STACK * pop() {
  if (stackEmpty()) return NULL;
  STACK * tos = requests;
  requests = requests->next;
  return tos;
}

int writeall(int fd, char *buf, int len){
  // adapted from beej's guide to network programming
  int total = 0;
  int bytes_left = len;
  int n;

  while (total < len) {
    n = write(fd, buf+total, bytes_left);
    if (n == -1) {
      perror("Socket %d returned error on write");
      return -1;
    }
    total += n;
    bytes_left -= n;
  }
  return 0;
}


int transferChunks(int serverFd, int clientFd, char * buf) {
  int testRead = 0;
  int testWrite = 0;
  char testBuf[1];
  int totalRead = 0;
  int totalWritten = 0;
  int readNow = 0;
  int writtenNow = 0;
  int done = 0;
  while (!done) {
    if ((testRead = recv(serverFd, testBuf, 1, MSG_PEEK | MSG_DONTWAIT)) > 0) {
      readNow = read(serverFd, buf, BUFSIZE);
      puts("going into testwrite");
      writtenNow = writeall(clientFd, buf, BUFSIZE);
      if (writtenNow < 0) {
        puts("writeall returned error, failing request"); // TODO - Write returnError method that can be called for all internal server errors
        return -1;
      }
      else {
        puts("writeall wrote something");
      }
      totalWritten += writtenNow;
    } else {
      done = 1;
    }
  }
  puts("Socket closed by server");
  printf("Total bytes written to client: %d\n", totalWritten);
  memset(buf, '\0', BUFSIZE);
  return 0;
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
  else {
    //puts("Waking up next thread");
    puts("Waking up all waiting threads");
    //pthread_cond_signal(&stackCond);
    pthread_cond_broadcast(&stackCond);
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

cacheResult cacheable(ReqInfo * reqInf) {
  return MISS;
}


void exitRequest(int connFd, ReqInfo * reqInf, char * request, struct addrinfo * servinfo) {
  long threadId = syscall(SYS_gettid);
  close(connFd);
  //free(host);
  if (reqInf != NULL) free(reqInf);
  printf("%lu Freed reqInf", threadId);
  if (request != NULL) free(request);
  printf("%lu Freed request", threadId);
  //if (servinfo != NULL) free(servinfo);
  if (servinfo != NULL) {
    printf("%lu About to free %p\n",threadId, servinfo);
    freeaddrinfo(servinfo);
  }
  printf("%lu exitRequest method exiting successfully", threadId);
}

ReqInfo * parseRequest(char * stringBuffer) {
  printf("[parseRequest()] About to parse request of stringBuffer %p\n", stringBuffer);
  printf("[parseRequest()] Length of request string: %d\n", (int)strlen(stringBuffer));
  if (strlen(stringBuffer) <= 4) {
    puts("[parseRequest()] Empty request string, returning NULL");
    return NULL;
  }
  puts("[parseRequest()] Request string:");
  printf("---:\n");
  printf("%s", stringBuffer);
  printf("---:\n");
  char *toBeFreed = stringBuffer;
  int reqType = 0, isHttps = 0, lineNum = 0;
  int isKeepAlive = 0;// TODO
  char * host, * line, * body, * headers;
  char * URI;
  while ((line = strsep(&stringBuffer, "\n")) != NULL) {
    lineNum ++;
    printf("Line %d : %s\n", lineNum, line);
    if (lineNum == 1) {
      //split by spaces
      char * word;
      char * originalLine = line;
      while ((word = strsep(&line, " ")) != NULL) {
        if (strcasecmp(word, "GET") == 0) {
          puts("[parseRequest()] GET request detected!");
          reqType = 0;
        }
        else if (strcasecmp(word, "CONNECT") == 0) {
          puts("CONNECT request detected!");
          reqType = 1;
          isHttps = 1;
        }
        else if (strcasecmp(word, "POST") == 0) {
          puts("POST request detected!");
          reqType = 2;
        }
        else if (strcasecmp(word, "HTTP/1.0\r") == 0) {
          puts("HTTP 1.0 request detected!");
        }
        else if (strcasecmp(word, "HTTP/1.1\r") == 0) {
          puts("HTTP 1.1 request detected!");
        }
        else {
          printf("Is this the URI? %s\n", word);
          URI = (char*) malloc((strlen(word) + 1) * sizeof(char));
          strcpy(URI, word);
          printf("URI saved as %s\n", URI);
        }
        // Reconstruct line
        /*printf("Reconstructing first line from %s\n", originalLine);*/
        if (line != NULL) *(originalLine + strlen(originalLine)) = ' ';
        /*printf("First line is now %s\n", originalLine);*/
      }
      /*puts("Reconstructed first line");*/
      /*printf("%s\n", originalLine);*/
    }
    else { // Different logic for parsing remaining lines as format is Header: <Header Value>
      if (strcasecmp(line, "\r") == 0) {
        puts("[parseRequest()] Detected END OF HEADERS!");
        break;
      }
      char * headerName = strsep(&line, ":");
      printf("[parseRequest()] Header: %s\n", headerName);
      printf("[parseRequest()] Header value: %s\n", line);

      // Logic for parsing hostname
      if (strcasecmp(headerName, "Host") == 0) {
        /*printf("[parseRequest()] Parsing host name from %s\n", line);*/
        char *hostName = strsep(&line, ":"); // TODO Sometimes port is mentioned as well
        printf("[parseRequest()] Deduced initial hostname name as %s\n", hostName); // + 1 to account for space after Host:
        host = (char*) malloc((strlen(hostName)+1) * sizeof(char));
        strncpy(host, hostName, strlen(hostName)+1);
        printf("Host after strncpy: %s\n", host);
        printf("Length of host after strncpy: %d\n", (int)strlen(host));
        if ((int) host[strlen(hostName)-1] < PRINTABLE_ASCII){
          //this only happens in http case because in https case carriage return is with port
          ///printf("Character currently at end of ");
          host[strlen(hostName)-1] = '\0';
        }
        host += 1;//remove space at front
        printf("[parseRequest()] Parsed final host name as %s\n", host);

        printf("[parseRequest()] Restoring host value header from %s\n", hostName);
        if (line != NULL) {
          printf("Is this port? %s\n", line);
          *(hostName + strlen(hostName)) = ':';
        }
        else {
          puts("[parseRequest()] No port mentioned in host header");
        }
      } else {
        printf("[parseRequest()] Header: %s\n", headerName);
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
  parsedRequest->URI = URI;
  printf("Set URI to %s\n", URI);
  return parsedRequest;
}


// Returns pointer to start of body
int parseResponse(char * responseHeaders, int *isChunked) {
  printf("About to parse response headers of stringBuffer %p\n", responseHeaders);
  printf("Length of request string: %d\n", (int)strlen(responseHeaders));
  if (strlen(responseHeaders) <= 4) {
    puts("Empty response headers, returning NULL");
    return -1;
  }
  puts("Response Headers:");
  printf("---:\n");
  printf("%s", responseHeaders);
  printf("---:\n");
  //int isKeepAlive = 0;// TODO
  //char * host, * line, * body, * headers;
  char contentLength[15] = "Content-Length\0";
  char cacheControl[14] = "Cache-Control\0";
  char transferEncoding[18] = "Transfer-Encoding\0";
  char * body;
  char * toBeFreed = responseHeaders;
  char * line;
  int lineNum = 0;
  int length = 0;
  while ((line = strsep(&responseHeaders, "\n")) != NULL) {
    lineNum ++;
    printf("Line : %s\n", line);
    if (strcasecmp(line, "\r") == 0) {
      puts("[parseResponse()] Detected END OF HEADERS!");
      break;
    }
    if (lineNum != 1) {
      char * headerName = strsep(&line, ":");
      printf("[parseResponse()] Header: %s\n", headerName);
      printf("[parseResponse()] Header value: %s\n", line);
      // TO-DO : Choose important headers, add parsing logic and pass them to forwardRequest method
      if (strcasecmp(headerName, contentLength) == 0) {
        puts("[parseResponse()] Detected content-length header");
        printf("String length from number onwards: %d\n",(int)strlen(line + 1));
        *(line + strlen(line) - 1) = '\0';
        printf("Trimmed number literal: %s\n", line + 1);
        length = atoi(line + 1);
        printf("Set content-length to %d\n", length);
        *(line + strlen(line)) = '\r';
        *isChunked = 0;
      }
      else if (strcasecmp(headerName, cacheControl) == 0) {
        // Logic for parsing cache-control policy
        printf("[parseResponse()] Parsing cache-control from %s\n", line);
        // cache-control can come in comma-delimited form like:
        // Cache-Control: no-cache, no-store, must-revalidate, max-age=0, proxy-revalidate, no-transform, private
        // make copy so that we dont have to restore commas...
        char *copyOfLine = malloc((strlen(line) + 1)*sizeof(char));
        char *cacheControlPolicy;
        while ((cacheControlPolicy = strsep(&copyOfLine, ",")) != NULL){
          printf("[parseResponse()] cache-control values include: %s \n", cacheControlPolicy);
        }
        *(line + strlen(line) - 1) = '\0';
        *(line + strlen(line)) = '\r';
      } else if (strcasecmp(headerName, transferEncoding) == 0){
        puts("[parseResponse()] Detected transfer-encoding header");
        char *encoding = line + 1;
        printf("transfer length encoding is %s", encoding);
        *isChunked = 1;
      }
      printf("[parseResponse()] Reconstructing header line from %s\n", headerName);
      *(headerName + strlen(headerName)) = ':';
      printf("[parseResponse()] Reconstructed header: %s\n", headerName);
    }
    printf("Reconstructing headers from %s\n", toBeFreed);
    *(toBeFreed + strlen(toBeFreed)) = '\n';
    //printf("Reconstructed headers as %s\n", toBeFreed);
  }
  int bodySizeLeft;
  puts("Restoring new line for separation line");
  *(line + 1) = '\n';
  if (responseHeaders != NULL) {
    printf("String buffer pointer: %p\n", responseHeaders);
    printf("Length of string buffer: %d\n", (int)strlen(responseHeaders));
    body = responseHeaders;
    //body = strsep(&responseHeaders, "\n"); // Now for body
    puts("Completed strsep");
    printf("Body pointer: %p\n", body);
    if (strlen(body) > 0) {
      printf("Body is %s\n", body);
      bodySizeLeft = length - strlen(body);
    }
    else {
      puts("Body is empty");
      bodySizeLeft = 0;
    }
  }
  else {
    puts("Body is empty");
    bodySizeLeft = 0;
  }
  *(toBeFreed + strlen(toBeFreed)) = '\n';
  printf("Recovered headers:\n%s", toBeFreed);
  puts("Merging headers and body for forwarding");
  if (bodySizeLeft) *(toBeFreed + strlen(toBeFreed)) = '\n';
  return bodySizeLeft;
}


// For non-encrypted request, bodySize is known
// For encrypted request, bodySize not known... So use separate method or same method with bodySize -1 and different logic?
int bufferedForward(int clientFd, int serverFd, char * serverBuff, int bodySize) {
  // Start by reading fro server
  int readNow = 0;
  int totalRead = 0;
  int writeNow = 0;
  int totalWritten = 0;
  int done = 0;
  printf("Thread %lu in bufferedForward method\n", syscall(SYS_gettid));
  while (totalWritten < bodySize && !done) {
    if ((readNow = read(serverFd, serverBuff, RESPONSE_HEADER_SIZE)) < 0) {
      perror("Error reading from client");
      done = 1;
    }
    else if (readNow == 0) {
      puts("No more bytes from origin server");
      done = 1; // Maybe not?
    }
    else {
      printf("Bytes read in this iteration: %d\n", readNow);
      totalRead += readNow;
      if ((writeNow = write(clientFd, serverBuff, readNow)) < 0) {
        perror("Error writing to client");
        done = 1;
      }
      else {
        printf("Bytes written to client in this iteration: %d\n", writeNow);
        totalWritten += writeNow;
      }
    }
  }
  printf("Total bytes read from server : %d\n", totalRead);
  printf("Total bytes written to client : %d\n", totalWritten);
  return totalWritten;
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
  hints.ai_protocol = 0;
  hints.ai_canonname = NULL;
  hints.ai_addr = NULL;
  hints.ai_next = NULL;
  const char * port = isHttps ? HTTPS : HTTP;
  int lookupResult;
  printf("Port: %s\n", port);
  printf("Address of hints: %p\n", &hints);
  printf("Address of servinfo: %p\n", &servinfo);
  printf("Host for servinfo: %s\n", host);
  printf("Length of hostname: %d\n", (int)strlen(host));
  if (((lookupResult = getaddrinfo(host, port, &hints, &servinfo))!=0)) {
    gai_strerror(lookupResult);
    puts("[forwardRequest()] Unable to getaddrinfo");
    printf("Servinfo: %p\n", servinfo);
    exitRequest(connFd, reqInf, request, NULL);
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
    writtenToClient = write(connFd, okResponse, strlen(okResponse));
    printf("Bytes written to client for CONNECT ack : %d\n", writtenToClient);
    puts("About to read on CONNECTed socket now");
    // Can overwrite string buffer for original CONNECT request
    memset(request, '\0', BUFSIZE);
    // First-pass : Single-shot read
    int readAgain = read(connFd, request, BUFSIZE);
    printf("Bytes read again from client: %d\n", readAgain);
    if (readAgain < 0) {
      perror("ERROR reading follow-up request from client after CONNECT");
      exitRequest(connFd, reqInf, request, servinfo);
      return;
    }
    int done = 0;
    int writtenToOriginServer = 0;
    int readFromOriginServer = 0;
    int totalWrittenToServer = 0;
    int totalWrittenToClient = writtenToClient;
    int testRead;
    int canWriteToServer = 1;
    int canWriteToClient = 0;
    while (!done) {
      if (canWriteToServer) {
        writtenToOriginServer = write(forwardSock, request, readAgain);
        printf("Relayed encrypted message to origin server, bytes written : %d\n", writtenToOriginServer);
        if (writtenToOriginServer < 0) {
          perror("ERROR forwarding encrypted response to origin server");
          exitRequest(connFd, reqInf, request, servinfo);
          return;
        }
        totalWrittenToServer += writtenToOriginServer;
      }
      memset(responseHeaderBuf, '\0', RESPONSE_HEADER_SIZE);
      //while ((readFromOriginServer = read(forwardSock, responseHeaderBuf, BUFSIZE)) == BUFSIZE) { // Havent read full data yet
      if ((testRead = recv(forwardSock, responseHeaderBuf, RESPONSE_HEADER_SIZE, MSG_PEEK | MSG_DONTWAIT)) == 0) {
        puts("Detected close of socket by server");
        exitRequest(connFd, reqInf, request, servinfo);
        return;
      }
      else if (testRead < 0) {
        //perror("Test read on server returns error:");
        canWriteToClient = 0;
      }
      else {
        if ((readFromOriginServer = read(forwardSock, responseHeaderBuf, BUFSIZE)) < 0) {
          perror("ERROR reading from server");
          //	perror("Server closed socket?");
          exitRequest(connFd, reqInf, request, servinfo);
          return;
        }
        canWriteToClient = 1;
      }
      //printf("Value returned by test read on server: %d\n", testRead);

      /*
         else if (readFromOriginServer == 0) {
         perror("Server closed socket?");
         printf("After %d bytes were written to him\n", totalWrittenToServer);
         done = 1;
         }
         */
      //printf("Read %d bytes from origin server, about to write to client and then re-read and re-write\n", readFromOriginServer);
      if (canWriteToClient) {
        if ((writtenToClient = write(connFd, responseHeaderBuf, readFromOriginServer)) < 0) {
          perror("Error forwarding encrypted response from server to client");
          exitRequest(connFd, reqInf, request, servinfo);
          return;
        }
        //printf("Wrote %d bytes to client\n", writtenToClient);
        totalWrittenToClient += writtenToClient;
      }
      memset(request, '\0', BUFSIZE);
      if ((testRead = recv(connFd, request, BUFSIZE, MSG_DONTWAIT | MSG_PEEK)) == 0) {
        puts("Detected close of socket by client");
        exitRequest(connFd, reqInf, request, servinfo);
        return;
      }
      else if (testRead < 0) {
        //perror("Test read on client returns error:");
        canWriteToServer = 0;
      }
      else {
        canWriteToServer = 1;
        //printf("Value returned by testRead on client: %d\n", testRead);
        if ((readAgain = read(connFd, request, BUFSIZE)) < 0) {
          perror("Error forwarding encrypted response from server to client");
          //exitRequest(connFd, reqInf, request, servinfo);
          //done = 1;
          //return;
          exitRequest(connFd, reqInf, request, servinfo);
          return;
        }
      }
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
    if ((readFromOrigin = read(forwardSock, responseHeaderBuf, RESPONSE_HEADER_SIZE)) < 0) {
      perror("Unable to read from origin server");
      exitRequest(connFd, reqInf, request, servinfo);
      return;
    }
    int isChunked = -1;
    int bodyLeft = parseResponse(responseHeaderBuf, &isChunked);
    printf("Successfully returned from parseResponse method with bodyLeft of %d\n", bodyLeft);
    // Write out buffer to client
    if ((writtenToClient = write(connFd, responseHeaderBuf, readFromOrigin)) < 0) {
      perror("Unable to do initial forwarding to client");
      exitRequest(connFd, reqInf, request, servinfo);
      return;
    }
    printf("Wrote %d of the initial bytes to client\n", writtenToClient);
    totalWrittenToClient += writtenToClient;
    if (bodyLeft > 0 && isChunked == 0) {
      printf("About to do buffered forwarding for %d\n bytes", bodyLeft);
      memset(request, '\0', BUFSIZE);
      memset(responseHeaderBuf, '\0', RESPONSE_HEADER_SIZE);
      totalWrittenToClient += bufferedForward(connFd, forwardSock, responseHeaderBuf, bodyLeft);
    }
    else {
      if (isChunked == 1){
        memset(request, '\0', BUFSIZE);
        int rc = transferChunks(forwardSock, connFd, request);
        if (rc == -1){
          puts("fatal error in transferChunks, closing sockets");
          exitRequest(connFd, reqInf, request, servinfo);
          return;
        }
      }
      else {
        puts("Completed forwarding in single write, no need for buffering");
      }
    }
    printf("Finished servicing request with total of %d bytes written\n", totalWrittenToClient);
    exitRequest(connFd, reqInf, request, servinfo);
    return;
  }


  void * serviceRequest() {
    long threadId = syscall(SYS_gettid);
    printf("Thread %ld here\n", threadId);
    int requestsServiced = 0;
    while(running) {
      printf("Thread %lu going to try and acuquire lock", threadId);
      pthread_mutex_lock(&stackMutex);
      printf("Thread %lu successfully acquired lock", threadId);
      while(stackEmpty() && running) {
        printf("Thread %ld checking status of stack\n", threadId);
        pthread_cond_wait(&stackCond, &stackMutex);
        printf("Thread %ld returned from wait\n", threadId);
      }
      if (running) {
        STACK * tos = pop();
        pthread_mutex_unlock(&stackMutex);
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
        cacheResult cacheRes;
        if (parsedReq == NULL) {
          puts("Bad request detected, returning error");
          int errorWritten;
          if ((errorWritten = write(connFd, BAD_REQUEST_RESPONSE, strlen(BAD_REQUEST_RESPONSE))) < 0) {
            puts("Error returning error response");
          }
          exitRequest(connFd, parsedReq, toBeFreed, NULL);
        } else if ((cacheRes = cacheable(parsedReq)) == HIT) {
          printf("Request %s is cacheable!\n", parsedReq->URI);
          exitRequest(connFd, parsedReq, toBeFreed, NULL);//TEMP
        }
        else {
          printf("Request %s is not cacheable because %d\n", parsedReq->URI, cacheRes);
          forwardRequest(connFd, parsedReq, toBeFreed); //Blocking : Opens socket and connection to origin server, writing from that socket to client socket
        }
        puts("Done servicing request");
      }
      //pthread_mutex_unlock(&stackMutex);
      else {
        printf("Thread %lu woken up, going to unlock and then try to exit\n", syscall(SYS_gettid));
        pthread_mutex_unlock(&stackMutex);
      }
      //pthread_mutex_unlock(&stackMutex);
    }
    printf("Thread %lu exiting after serving %d requests\n", threadId, requestsServiced);
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
    if (DEBUG) puts("[quit()] Main thread waiting for pool threads to exit");
    pthread_cond_wait(&exitCond, &exitMutex);
    if (DEBUG) printf("[quit()] Main thread about to exit\n");
    freeAll();
    close(listenfd);
    pthread_mutex_unlock(&exitMutex);
    pthread_mutex_destroy(&stackMutex);
    pthread_mutex_destroy(&exitMutex);
    pthread_cond_destroy(&stackCond);
    pthread_cond_destroy(&exitCond);
    if (DEBUG) puts("[quit()] Freed all resources");
    if (DEBUG) printf("[quit()] Total requests serviced : %d\n", serviced);
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
