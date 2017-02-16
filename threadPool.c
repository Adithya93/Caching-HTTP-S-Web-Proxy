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
#include <time.h>
#include "./cache.c"
#include "./timeparse.c"
#include "./logging.c"


#define BUFSIZE 1024
#define PORTNAME 80
#define POOLSIZE 10
#define RESPONSE_HEADER_SIZE 8192
#define HTTP_PORT 80
#define HTTPS_PORT 443
#define PRINTABLE_ASCII 32
#define DEBUG 1

const char *LOGFILE_PATH = "/var/log/erss-proxy.log";
const char *HTTP = "http";
const char *HTTPS = "https";
const char *BAD_REQUEST_RESPONSE = "HTTP/1.1 400 Bad Request\r\n\0";


typedef struct stack {
  int fd;
  //struct sockaddr * sockPtr;
  //socklen_t * addrLen;
  struct stack * next;
} STACK;

typedef struct reqInfo {
  int reqType;
  int isHttps;
  char *host;
  char *URI;
  char *reqLine;
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
  // used to track all incoming requests
  if (stackEmpty()) return NULL;
  STACK * tos = requests;
  requests = requests->next;
  return tos;
}

int writeall(int fd, char *buf, int * len){
  // adapted from beej's guide to network programming
  int total = 0;
  int bytes_left = *len;
  int n;

  while (total < *len) {
    n = write(fd, buf+total, bytes_left);
    if (n == -1) {
      perror("Socket %d returned error on write");
      return -1;
    }
    
    total += n;
    bytes_left -= n;
  }
  *len = total;
  return 0;
}


int transferChunks(int serverFd, int clientFd, char * buf, char ** cacheBuffPtr, int * canCache) {
  // handles transfer encoding=chunked as well as revalidation responses from a server (that are not 304 not modified).
  int testRead = 0;
  int testWrite = 0;
  char testBuf[1];
  int totalRead = 0;
  int totalWritten = 0;
  int readNow = 0;
  int writtenNow = 0;
  int done = 0;
  char * cacheBuff = *cacheBuffPtr;
  int currentCacheBuffCap = RESPONSE_HEADER_SIZE;
  int currentCacheBuffSize = (int)strlen(cacheBuff);
  int cacheFail = *canCache ? 0 : 1;
  int amountToWrite = BUFSIZE;
  int grace = 10000;
  puts("About to start transferring chunks");
  while (grace > 0 && (testRead = recv(serverFd, testBuf, 1, MSG_PEEK | MSG_DONTWAIT)) != 0) {
  //while (grace > 0 && (testRead = recv(serverFd, buf, BUFSIZE, MSG_PEEK | MSG_DONTWAIT)) != 0) { 
    if (testRead < 0) {
      usleep(200);
      grace --;
      //perror("Error returned by testRead in transferChunks");
      continue;
    } 
    readNow = read(serverFd, buf, BUFSIZE);
    if (readNow < 0) {
      perror("Unable to read from server in transferChunks");
      *canCache = 0;
      return -1;
    }
    //puts("going into writeall");
    amountToWrite = readNow;
    writtenNow = writeall(clientFd, buf, &amountToWrite);
    if (writtenNow < 0) {
      puts("Writeall returned error");
      *canCache = 0;
      return -1;
    }
    else if (!cacheFail) {
      int cacheBuffLeft = currentCacheBuffCap - currentCacheBuffSize;
      if (writtenNow > cacheBuffLeft) {
	printf("Current capacity of cache buffer: %d, current size of cache buffer : %d, about to add %d bytes\n", currentCacheBuffCap, currentCacheBuffSize, writtenNow);
	char * oldCacheBuff = cacheBuff;
        if (!(cacheBuff = realloc(cacheBuff, 2 * currentCacheBuffCap * sizeof(char)))) {
	  printf("Unable to reallocate cache buffer to size %d\n", 2 * currentCacheBuffCap);
	  cacheFail = 1;
        }
	currentCacheBuffCap *= 2;
        printf("Reallocated cache buffer from %p to %p, new capacity %d\n", oldCacheBuff, cacheBuff, currentCacheBuffCap);
      }
      strcat(cacheBuff, buf); // Only add for successful writes, and only cache at the end if entire write was successful
      currentCacheBuffSize += writtenNow;
    }
    totalWritten += amountToWrite;
    amountToWrite = BUFSIZE;
  // else {
  // done = 1;
  //}
    memset(buf, '\0', BUFSIZE);
  }
  printf("Value of grace left in transferChunks: %d\n", grace);
  //puts("Socket closed by server");
  printf("Total bytes written to client: %d\n", totalWritten);
  memset(buf, '\0', BUFSIZE);
  if (!cacheFail) {
    *cacheBuffPtr = cacheBuff;
    *canCache = 1;
    //return 0;
    //return totalWritten;
  }
  else {
    *canCache = 0;
    //puts("Write to client succeeded but caching failed as response buffer was too big");
    //return -2;
  }
  printf("Final chunk written: %s\n", buf);
  return totalWritten;
}

// Only called by main thread whose on main method is not synchronized, therefore needs locking here
void push(int fd) {
  pthread_mutex_lock(&stackMutex);
  STACK * newReq = (STACK*) malloc(sizeof(STACK));
  newReq->fd = fd;
  //newReq->sockPtr = SA;
  //newReq->addrLen = AL;
  newReq->next = requests;
  requests = newReq;
  pthread_cond_signal(&stackCond);
  pthread_mutex_unlock(&stackMutex);
}

void incrExit(int reqsServiced) {
  // threads call this when they terminate.
  pthread_mutex_lock(&exitMutex);
  long threadId = syscall(SYS_gettid);
  if (++exited == POOLSIZE) {
    printf("Last thread %ld exiting, about to signal main thread\n", threadId);
    pthread_cond_signal(&exitCond);
  }
  else {
    pthread_cond_broadcast(&stackCond);
  }
  printf("Thread %ld is %d to exit\n", threadId, exited);
  serviced += reqsServiced;
  pthread_mutex_unlock(&exitMutex);
}


// Check if in cache, not expired and doesn't need revalidation
cacheResult cacheable(ReqInfo * reqInf, char ** cacheData) {
  //char * result = get(reqInf->URI);
  NODE * result = get(reqInf->URI);
  if (result != NULL) {
    //printf("About to store returned address %p into storing pointer %p\n", result, cacheData);
    printf("Retrieved node of address %p\n", result);
    // Check revalidation and expiry
    struct tm * currentTime = getCurrentTime(); // MUST FREE after checking
    printf("Current time pointer : %p\n", currentTime);
    printf("Result's info pointer: %p\n", result->info);
    if ((result->info->expiryTime != NULL) && (timeAgtB(currentTime, result->info->expiryTime) >= 0)) {
      printf("Detected expiry of cached data for URI %s\n", reqInf->URI);
      free(currentTime);
      return EXPIRED;
    }
    else {
      puts("Not expired");
    }
    if (result->info->needsRevalidation) {
      printf("Detected need for revalidation for URI %s\n", reqInf->URI);
      free(currentTime);
      *cacheData = result->val; // In case revalidation not required, can directly use this result
      return REVALIDATE;
    }
    else {
      puts("Revalidation not needed for this URI");
    }
    *cacheData = result->val;
    printf("Retrieved data of length %d from cache!\n", (int)strlen(result->val));
    free(currentTime);
    return HIT;
  }
  else return MISS;
}


void exitRequest(int connFd, ReqInfo * reqInf, char * request) {
  // when a request is serviced, free all resources.
  puts("In exitRequest method");
  long threadId = syscall(SYS_gettid);
  close(connFd);
  //free(host);
  if (reqInf != NULL) {
    printf("Freeing host %s\n", reqInf->host);
    free(reqInf->host);
    printf("Freeing URI %s\n", reqInf->URI);
    free(reqInf->URI);//TODO free fields inside?
    printf("About to free reqLine %s\n",reqInf-> reqLine);
    free(reqInf->reqLine);
    printf("%lu Freed reqInf", threadId);
  }
  if (request != NULL) free(request);
  printf("%lu Freed request", threadId);
  //if (servinfo != NULL) free(servinfo);
  /*
     if (servinfo != NULL) {
     printf("%lu About to free %p\n",threadId, servinfo);
     freeaddrinfo(servinfo);
     }
     */
  printf("%lu exitRequest method exiting successfully", threadId);
}

ReqInfo * parseRequest(char * stringBuffer, char *UID) {
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
  char * host, * line, * body, * headers;
  char * URI;
  ReqInfo * parsedRequest = (ReqInfo *) malloc(sizeof(ReqInfo));
  while ((line = strsep(&stringBuffer, "\n")) != NULL) {
    lineNum ++;
    printf("Line %d : %s\n", lineNum, line);
    if (lineNum == 1) {
      char * word;
      char * originalLine = line;
      char *reqLinee = malloc((strlen(line)+1) * sizeof(char));
      memset(reqLinee, '\0', (strlen(line)+1)*sizeof(char));
      strcpy(reqLinee, line);
      parsedRequest->reqLine = reqLinee; 
      printf("REQLINE AFTER PARSE %s\n", parsedRequest->reqLine);
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
      if (strncasecmp(line, "\r", 1) == 0) {
        puts("[parseRequest()] Detected END OF HEADERS!");
        break;
      }
      char * headerName = strsep(&line, ":");
      printf("[parseRequest()] Header: %s\n", headerName);
      printf("[parseRequest()] Header value: %s\n", line);

      // Logic for parsing hostname
      if (strcasecmp(headerName, "Host") == 0) {
        printf("[parseRequest()] Parsing host name from %s\n", line);
        char *hostName = strsep(&line, ":"); // TODO Sometimes port is mentioned as well
        printf("[parseRequest()] Deduced initial hostname name as %s\n", hostName + 1); // + 1 to account for space after Host:
        host = (char*) malloc((strlen(hostName + 1)+1) * sizeof(char));
        strncpy(host, hostName + 1, strlen(hostName + 1) + 1);
        printf("Host after strncpy: %s\n", host);
        printf("Length of host after strncpy: %d\n", (int)strlen(host));
        //if ((int) host[strlen(hostName)-1] < PRINTABLE_ASCII){
	printf("ASCII value of last character of host: %d", (127 & (int)host[strlen(host) - 1]));
	if ((int)host[strlen(host) - 1] < PRINTABLE_ASCII) {
	//this only happens in http case because in https case carriage return is with port
          ///printf("Character currently at end of ");
          //host[strlen(hostName)-1] = '\0';
	  printf("Stripping last character : %c\n", host[strlen(host) - 1]);
	  host[strlen(host) - 1] = '\0';
	}

	// This line caused invalid free
        //host += 1;//remove space at front
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


int parseCacheHeaders(char * cacheHeaders, CacheInfo * cacheInfo) {
  printf("About to parse cache headers %s\n", cacheHeaders);
  char * cacheControlPolicy;
  char mustRevalidate[16] = "must-revalidate\0";
  char proxyRevalidate[17] = "proxy-revalidate\0";
  char noCache[9] = "no-cache\0";
  char noStore[9] = "no-store\0";
  char private[8] = "private\0";
  char maxAge[9] = "max-age=\0";
  int mustRevalidateLen = 15;
  int maxAgeLen = 8;

  //int count = 0;
  cacheInfo->needsRevalidation = 0; // Set default value in case header doesnt exist
  cacheInfo->expiryTime = NULL;
  while ((cacheControlPolicy = strsep(&cacheHeaders, ",")) != NULL) {
    //if (!count) cacheControlPolicy += 1;
    cacheControlPolicy += 1;
    if (*(cacheControlPolicy + (int)strlen(cacheControlPolicy) - 1) == '\r') {
      *(cacheControlPolicy + (int)strlen(cacheControlPolicy) - 1) = '\0';
    }
    printf("Cache-Control header value: %s\n", cacheControlPolicy);
    if (strcasecmp(cacheControlPolicy, noStore) == 0 || strcmp(cacheControlPolicy, private) == 0) {
      printf("Detected forbidding cache-policy: %s\n", cacheControlPolicy);
      return 0;
    }
    else if (strncasecmp(cacheControlPolicy, mustRevalidate, mustRevalidateLen) == 0 || strcmp(cacheControlPolicy, proxyRevalidate) == 0 || strcmp(cacheControlPolicy, noCache) == 0) {
      puts("Must Revalidate!");
      cacheInfo->needsRevalidation = 1;
    }
    else if (strncasecmp(cacheControlPolicy, maxAge, maxAgeLen) == 0) {
      printf("Detected max-age string : %s\n", maxAge);
      char * numStart = cacheControlPolicy + maxAgeLen;
      printf("NumStart: %s\n", numStart);
      long expiry = strtol(numStart, NULL, 10);
      printf("Expiry in seconds : %lu\n", expiry);
      struct tm * expiryTm = getTimeFromExpiry(expiry);
      cacheInfo->expiryTime = expiryTm;
    }
    else {
      printf("Unknown cache-control header: %s\n", cacheControlPolicy);
    }
    if (*(cacheControlPolicy + (int)strlen(cacheControlPolicy) - 1) == '\r') {
      printf("Detected end of cache control headers!");
      break;
    }
    //count ++;
  }
  // TEMP
  return 1;
}


// Returns pointer to start of body
int parseResponse(char * responseHeaders, int *isChunked, CacheInfo * cacheInfo, int * canCache) {
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
  //char * host, * line, * body, * headers;
  char contentLength[15] = "Content-Length\0";
  char cacheControl[14] = "Cache-Control\0";
  char transferEncoding[18] = "Transfer-Encoding\0";
  char etag[5] = "ETag\0";
  char * body;
  char * toBeFreed = responseHeaders;
  char * line;
  int lineNum = 0;
  int length = 0;
  int hasCacheControl = 0;
  while ((line = strsep(&responseHeaders, "\n")) != NULL) {
    lineNum ++;
    printf("Line : %s\n", line);
    if (strcasecmp(line, "\r") == 0) {
      puts("[parseResponse()] Detected END OF HEADERS!");
      printf("responseHeaders ptr is currently %p\n", responseHeaders);
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
        hasCacheControl = 1;
        // Logic for parsing cache-control policy
        printf("[parseResponse()] Parsing cache-control value from %s\n", line);
        // cache-control can come in comma-delimited form like:
        // Cache-Control: no-cache, no-store, must-revalidate, max-age=0, proxy-revalidate, no-transform, private
        // make copy so that we dont have to restore commas...
        char *copyOfLine = malloc((strlen(line) + 1)*sizeof(char));
        char *cacheControlPolicy;

        strncpy(copyOfLine, line, strlen(line) + 1);


        /* Move to parseCacheControl method?
           while ((cacheControlPolicy = strsep(&copyOfLine, ",")) != NULL){
           printf("[parseResponse()] cache-control values include: %s \n", cacheControlPolicy);
           }
           */

        *canCache = parseCacheHeaders(copyOfLine, cacheInfo);
        if (!(*canCache)) {

          puts("Origin server forbids caching, setting cache flag to 0");
        }
        else {
          puts("Cache-control of origin server allows caching");
        }

        //*(line + strlen(line) - 1) = '\0';
        //*(line + strlen(line)) = '\r';
      } else if (strcasecmp(headerName, transferEncoding) == 0){
        puts("[parseResponse()] Detected transfer-encoding header");
        char *encoding = line + 1;
        printf("transfer length encoding is %s", encoding);
        *isChunked = 1;
      } else if (strncasecmp(headerName, etag, 4) == 0){
        puts("[parseResponse()] etag header found");
        char *copyOfLine = malloc(strlen(line)*sizeof(char));
        strncpy(copyOfLine, line+1, strlen(line)-2);
        copyOfLine[strlen(line)-2] = '\0';
        printf("etag was found to be %s\n",copyOfLine);
        cacheInfo->eTag = copyOfLine; 
      } else {
        printf("unrecognized header type: %s\n", headerName);
      }
      printf("[parseResponse()] Reconstructing header line from %s\n", headerName);
      *(headerName + strlen(headerName)) = ':';
      printf("[parseResponse()] Reconstructed header: %s\n", headerName);
    }     printf("Reconstructing headers from %s\n", toBeFreed);
    *(toBeFreed + strlen(toBeFreed)) = '\n';
    //printf("Reconstructed headers as %s\n", toBeFreed);
  }
  int bodySizeLeft;
  puts("Restoring new line for separation line");
  *(line + 1) = '\n';
  printf("Joined response is now %s\n", toBeFreed);
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
  if (!hasCacheControl) *canCache = 0;
  return bodySizeLeft;
}


// For non-encrypted request, bodySize is known
// For encrypted request, bodySize not known... So use separate method or same method with bodySize -1 and different logic?
int bufferedForward(int clientFd, int serverFd, char * serverBuff, char ** cacheBuffPtr, int bodySize, int * canCache) {
  // Start by reading fro server
  int readNow = 0;
  int totalRead = 0;
  int writeNow = 0;
  int totalWritten = 0;
  int done = 0;
  int currentCacheBuffCap = RESPONSE_HEADER_SIZE;
  char * cacheBuff = *cacheBuffPtr;
  int currentCacheBuffSize = (int)strlen(cacheBuff);
  int cacheFail = *canCache ? 0 : 1;
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
        if (!cacheFail) {
          int cacheBuffLeft = currentCacheBuffCap - currentCacheBuffSize;
          if (writeNow > cacheBuffLeft) {
            printf("Current capacity of cache buffer: %d, current size of cache buffer : %d, about to add %d bytes\n", currentCacheBuffCap, currentCacheBuffSize, writeNow);
            char * oldCacheBuff = cacheBuff;
            if (!(cacheBuff = realloc(cacheBuff, 2 * currentCacheBuffCap * sizeof(char)))) {
              printf("Unable to reallocate cache buffer to size %d\n", 2 * currentCacheBuffCap);
              cacheFail = 1;
            }
            currentCacheBuffCap *= 2;
            printf("Reallocated cache buffer from %p to %p, new capacity %d\n", oldCacheBuff, cacheBuff, currentCacheBuffCap);
          }
          strcat(cacheBuff, serverBuff); // Only add for successful writes, and only cache at the end if entire write was successful
          currentCacheBuffSize += writeNow;
        }
      }
    }
    memset(serverBuff, '\0', RESPONSE_HEADER_SIZE);
  }
  printf("Total bytes read from server : %d\n", totalRead);
  printf("Total bytes written to client : %d\n", totalWritten);
  if (!cacheFail) {
    *canCache = 1;
    *cacheBuffPtr = cacheBuff; // Inform caller of new ptr
  }
  else puts("Not caching response as it was too big");
  return totalWritten;
}


int connectToServer(ReqInfo * reqInf) {
  int reqType = reqInf -> reqType;
  int isHttps = reqInf -> isHttps;
  char * host = reqInf -> host;
  //printf("Forwarding request from socket %d, reqType %d, isHttps %d, host %s\n", connFd, reqType, isHttps, host);
  int forwardSock = socket(AF_INET, SOCK_STREAM, 0);
  if (forwardSock < 0) {
    perror("ERROR opening forwarding socket");
    return -1;
  }
  struct addrinfo hints;
  struct addrinfo * servinfo;
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
    freeaddrinfo(servinfo);
    return -1;
  }
  printf("Successfully obtained server info for %s, lookup result is %d\n", host, lookupResult);
  printf("Servinfo now: %p\n", servinfo);
  printf("Forward sock: %d, ai_addr: %p, addrlen: %d\n", forwardSock, servinfo->ai_addr, (int)(servinfo->ai_addrlen));
  if (connect(forwardSock, servinfo->ai_addr, servinfo->ai_addrlen) < 0) {
    perror("ERROR, unable to connect to host");
    fprintf(stderr, "Unable to connect to host %s\n", host);
    freeaddrinfo(servinfo);
    return -1;
  }
  freeaddrinfo(servinfo);
  return forwardSock;
}


int getRevalidation(int clientFd, ReqInfo * reqInf, char * URI) {
  //  GET /foo/bar/fun.html HTTP/1.1
  //  If-None-Match: "1aa008f-2d-50a3559482cc0"
  //  Host: www.origin.com

  // reqInf has host, URI and reqLine ptrs
  // Lookup URI in cache, retrieve node
  NODE * resultNode = get(URI);
  if (resultNode == NULL) { // GG
    printf("ERROR: Node unexpectedly missing for %s\n", URI);
    return -1;
  }

  // Retrieve ETag
  puts("getting etag");
  char * eTag = resultNode->info->eTag;
  if (eTag == NULL) { // Shouldnt happen, but if so treat as needing to revalidate
    printf("ERROR: eTag unexpectedly missing for %s\n", URI); // TEMP
    return -1;
  }
  printf("Retrieved eTag as %s\n", eTag);

  // Retrieve Host
  char * host = resultNode->host;
  if (host == NULL) { // Also shouldn't happen
    printf("ERROR: host unexpectedly missing for %s\n", host);
    return -1;
  }
  printf("Retrieved host as %s\n", host);
  char * revalidationTemplate = "GET %s HTTP/1.1\r\nIf-None-Match: %s\r\nHost: %s\r\n\r\n\0";
  //char revalidationHeaders[BUFSIZE];
  char revalidationHeaders[RESPONSE_HEADER_SIZE];
  //memset(revalidationHeaders, '\0', BUFSIZE);
  memset(revalidationHeaders, '\0', RESPONSE_HEADER_SIZE);
  if (sprintf(revalidationHeaders, revalidationTemplate, URI, eTag, host) < 0) {
    puts("GG I fucked up");
    return -1;
  }
  int serverSock;
  if ((serverSock = connectToServer(reqInf)) < 0) {
    printf("Unable to connect to server %s for revalidation\n", host);
    return -1;
  }
  // Write revalidationHeaders to server
  int writtenToServer = strlen(revalidationHeaders);
  if (writeall(serverSock, revalidationHeaders, &writtenToServer)) {
    puts("Unable to write revalidation headers to server");
    return -1;
  }
  printf("Total bytes written to server : %d\n", writtenToServer);
  // Read initial response from server socket in a loop until done
  int testRead;
  char testBuf[1];
  int readNow = 0;
  int totalRead = 0;
  int grace = 5000;

  char canCache[26] = "HTTP/1.1 304 Not Modified\0";

  int targetLen = strlen(canCache); // Only want to read this much and do strncasecmp before deciding what to do next
  
  //memset(revalidationHeaders, '\0', BUFSIZE);
  memset(revalidationHeaders, '\0', RESPONSE_HEADER_SIZE);
  //while (totalRead < BUFSIZE && (testRead = recv(serverSock, testBuf, 1, MSG_PEEK | MSG_DONTWAIT)) > 0) {
  while (grace > 0 && (totalRead < targetLen) && (testRead = recv(serverSock, testBuf, 1, MSG_PEEK | MSG_DONTWAIT)) != 0) {
    if (testRead > 0) {
      readNow = read(serverSock, revalidationHeaders, BUFSIZE);
      if (readNow < 0) {
        perror("Error reading from server in revalidation");
        return -1;
      }
      printf("Read %d bytes of initial revalidation response from server\n", readNow);
      totalRead += readNow;
    }
    else {
      perror("Test read in revalidation returns error");
      grace --;
      usleep(200);
    }
  }
  printf("Value of grace left for reading server revalidation initial response: %d\n", grace);
  printf("Bytes read from server: %d\n", totalRead);
  // Parse this and figure out if use cached or not
  //char * firstLine = strsep(&revalidationHeaders, "\r");
  //printf("First line: %s\n", firstLine);
  //304 Not Modified
  //char canCache[27] = "HTTP/1.1 304 Not Modified\0";
  printf("revalidation headers: %s", revalidationHeaders);
  if (strncasecmp(revalidationHeaders, canCache, 25) == 0) {
    puts("Detected Not Modified!");
    return 0;
  }
  else {
    puts("GG need to forward everything");
    // First write what was already read to client
    int totalWritten = 0;
    int writeNow = 0;
    int initialWritten = totalRead;
    if (writeall(clientFd, revalidationHeaders, &initialWritten)) {
      puts("Error writing initial revalidation data to client");
      return -1;
    }
    totalWritten = initialWritten;
    printf("Written %d initial bytes to client\n", initialWritten);
    char * cacheBuff = (char *) malloc((RESPONSE_HEADER_SIZE + 1) * sizeof(char));
    memset(cacheBuff, '\0', RESPONSE_HEADER_SIZE+1);
    int shouldCache = 1;
    int transferResult;
    strcpy(cacheBuff, revalidationHeaders);
    memset(revalidationHeaders, '\0', RESPONSE_HEADER_SIZE);
    puts("About to call transferChunks from getRevalidation method to transfer remaining response");
    if ((transferResult = transferChunks(serverSock, clientFd, revalidationHeaders, &cacheBuff, &shouldCache)) < 0) {
      puts("Unable to transfer all data to client");
      return -1;
    }
    totalWritten += transferResult;
    // check if should cache, and do so
    if (shouldCache) {
      puts("About to cache new data from server");
      printf("Saving cached data for URI %s of length %d\n", reqInf->URI, (int)strlen(cacheBuff));
      put(resultNode->key, cacheBuff, resultNode->host, resultNode->info); // Only cacheBuff new, and info ptr modified with its struct expiry * struct, rest same
    }
    else {
      puts("Write to client succeeded but not caching, freeing cacheBuffer");
      free(cacheBuff);
    }
  }
  return 1;
}



void forwardRequest(int connFd, ReqInfo * reqInf, char * request, char* UID) {

  char *logString = malloc(120*sizeof(char));
  memset(logString, '\0', 120*sizeof(char));
  sprintf(logString, "%s: Requesting %s from %s", UID, reqInf->reqLine,reqInf->host);
  logpush(logString);
  free(logString);

  int forwardSock;
  int reqType = reqInf -> reqType;
  char responseHeaderBuf[RESPONSE_HEADER_SIZE];
  if ((forwardSock = connectToServer(reqInf)) < 0) {
    printf("Unable to connect to host %s\n", reqInf->host);
    exitRequest(connFd, reqInf, request);
  }
  printf("Successfully connected to %s\n", reqInf->host);
  int writtenToClient = 0;

  // HANDLE CONNECT REQUEST
  if (reqType == 1) {
    char okResponse[94] = "HTTP/1.1 200 Connection established\r\nConnection: Keep-Alive\r\nProxy-Connection: Keep-Alive\r\n\r\n\0";
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
      exitRequest(connFd, reqInf, request);
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
          exitRequest(connFd, reqInf, request);
          return;
        }
        totalWrittenToServer += writtenToOriginServer;
      }
      memset(responseHeaderBuf, '\0', RESPONSE_HEADER_SIZE);
      if ((testRead = recv(forwardSock, responseHeaderBuf, RESPONSE_HEADER_SIZE, MSG_PEEK | MSG_DONTWAIT)) == 0) {
        puts("Detected close of socket by server");
        char *logString = malloc(40*sizeof(char));
        memset(logString, '\0', 40*sizeof(char));
        sprintf(logString, "%s: Tunnel closed.", UID);
        logpush(logString);
        free(logString);

        exitRequest(connFd, reqInf, request);
        return;
      }
      else if (testRead < 0) {
        canWriteToClient = 0;
      }
      else {
        if ((readFromOriginServer = read(forwardSock, responseHeaderBuf, BUFSIZE)) < 0) {
          perror("ERROR reading from server");
          exitRequest(connFd, reqInf, request);
          return;
        }
        canWriteToClient = 1;
      }
      if (canWriteToClient) {
        if ((writtenToClient = write(connFd, responseHeaderBuf, readFromOriginServer)) < 0) {
          perror("Error forwarding encrypted response from server to client");
          exitRequest(connFd, reqInf, request);
          return;
        }
        //printf("Wrote %d bytes to client\n", writtenToClient);
        totalWrittenToClient += writtenToClient;
      }
      memset(request, '\0', BUFSIZE);
      if ((testRead = recv(connFd, request, BUFSIZE, MSG_DONTWAIT | MSG_PEEK)) == 0) {
        puts("Detected close of socket by client");
        char *logString = malloc(40*sizeof(char));
        memset(logString, '\0', 40*sizeof(char));
        sprintf(logString, "%s: Tunnel closed.", UID);
        logpush(logString);
        free(logString);
        exitRequest(connFd, reqInf, request);
        return;
      }
      else if (testRead < 0) {
        canWriteToServer = 0;
      }
      else {
        canWriteToServer = 1;
        if ((readAgain = read(connFd, request, BUFSIZE)) < 0) {
          perror("Error forwarding encrypted response from server to client");
          exitRequest(connFd, reqInf, request);
          return;
        }
      }
    }
    puts("TUNNELING COMPLETE");
    exitRequest(connFd, reqInf, request);
    return;
  }
  // Handle non-connect requests 
  // First-pass : Try to write entire buffer in a single syscall
  int written;
  printf("Length of request: %d\n", (int)strlen(request));
  written = write(forwardSock, request, strlen(request));
  printf("Bytes written to origin server %s: %d\n", reqInf->host, written);
  //bzero(responseHeaderBuf, RESPONSE_HEADER_SIZE);
  memset(responseHeaderBuf, '\0', RESPONSE_HEADER_SIZE);
  char *responseHeaderPtr = responseHeaderBuf;
  int readFromOrigin = 0;
  int totalReadFromOrigin = 0;
  int totalWrittenToClient = 0;
  char testBuf[1];
  int testRead;
  int done = 0;
  int grace = 10000;
  //while ((totalReadFromOrigin < RESPONSE_HEADER_SIZE) && ((testRead = recv(forwardSock, testBuf, 1, MSG_PEEK | MSG_DONTWAIT)) > 0)) {
  while ((grace > 0) && (totalReadFromOrigin < RESPONSE_HEADER_SIZE - BUFSIZE) && (testRead = recv(forwardSock, testBuf, 1, MSG_PEEK | MSG_DONTWAIT)) != 0) {
    if (testRead > 0) {
      //if ((readFromOrigin = read(forwardSock, responseHeaderPtr, RESPONSE_HEADER_SIZE)) < 0) {
      if ((readFromOrigin = read(forwardSock, responseHeaderPtr, BUFSIZE)) < 0) {
        perror("Unable to read from origin server");
        exitRequest(connFd, reqInf, request);
        return;
      }
      totalReadFromOrigin += readFromOrigin;
      responseHeaderPtr += readFromOrigin;
    }
    else if (testRead < 0) {
      //perror("Test read on server in forwardrequest returns error");
      grace --;
      usleep(20);
    }
    else {
      done = 1;
    }
  }

  char *logString2 = malloc(120*sizeof(char));
  memset(logString2, '\0', 120*sizeof(char));
  sprintf(logString2, "%s: received response from %s.", UID, reqInf->host);
  logpush(logString2);
  free(logString2);

  printf("Grace value at the end: %d\n", grace);
  printf("Size of initial read: %d\n", totalReadFromOrigin);
  
  int isChunked = -1;
  CacheInfo * cacheInfo = (CacheInfo*) malloc(sizeof(CacheInfo));
  int serverAllowsCaching = 1;
  int bodyLeft = parseResponse(responseHeaderBuf, &isChunked, cacheInfo, &serverAllowsCaching);
  printf("Successfully returned from parseResponse method with bodyLeft of %d\n", bodyLeft);

  serverAllowsCaching ? puts("Server allows caching") : puts("Server forbids caching");
  // Write out buffer to client
  char * cacheBuff = (char *)malloc((RESPONSE_HEADER_SIZE + 1) * sizeof(char));
  memset(cacheBuff, '\0', RESPONSE_HEADER_SIZE);
  char * cacheKey = (char *)malloc((strlen(reqInf->URI) + 1) * sizeof(char));
  char * cacheHost = (char *)malloc((strlen(reqInf->host) + 1) * sizeof(char));
  /* SHOULD USE WRITEALL INSTEAD
  if ((writtenToClient = write(connFd, responseHeaderBuf, readFromOrigin)) < 0) {
    perror("Unable to do initial forwarding to client");
    exitRequest(connFd, reqInf, request);
    return;
  }
  */
  //int initialWritten = readFromOrigin;
  writtenToClient = totalReadFromOrigin;
  if (writeall(connFd, responseHeaderBuf, &writtenToClient) < 0) {
    puts("Error forwarding initial bytes to client after parsing response");
    exitRequest(connFd, reqInf, request);
    puts("Freeing cacheInfo and cacheBuffer for premature exit");
    free(cacheInfo);
    free(cacheBuff);
    return;
  }
  printf("Wrote %d of the initial bytes to client\n", writtenToClient);
  if (serverAllowsCaching) {
    strncpy(cacheBuff, responseHeaderBuf, writtenToClient);
    //cacheInfo->key = (char *)malloc((strlen(reqInf->URI) + 1) * sizeof(char));
    strncpy(cacheKey, reqInf->URI, strlen(reqInf->URI) + 1);
    printf("Set cache URI key to %s\n", cacheKey);
    //cacheInfo->host = (char *)malloc((strlen(reqInf->host) + 1) * sizeof(char));
    strncpy(cacheHost, reqInf->host, strlen(reqInf->host) + 1);
    printf("Set cache URI host to %s\n", cacheHost);
  }

  totalWrittenToClient += writtenToClient;
  //int canCache = 0;
  int canCache = serverAllowsCaching;
  if (bodyLeft > 0 && isChunked == 0) {
    printf("About to do buffered forwarding for %d\n bytes", bodyLeft);
    memset(request, '\0', BUFSIZE);
    memset(responseHeaderBuf, '\0', RESPONSE_HEADER_SIZE);
    //int canCache = 0;
    totalWrittenToClient += bufferedForward(connFd, forwardSock, responseHeaderBuf, &cacheBuff, bodyLeft, &canCache);
    if (!canCache) {
      serverAllowsCaching ? puts("Unable to cache as response was too big") : puts("Unable to cache as server forbids it");
      //puts("Freeing cache buffer");
      char *logString = malloc(120*sizeof(char));
      memset(logString, '\0', 120*sizeof(char));
      if (serverAllowsCaching) sprintf(logString, "%s: not cacheable because response is too big.", UID);
      else if (!serverAllowsCaching) sprintf(logString, "%s: not cacheable because server forbids it.", UID);
      logpush(logString);
      free(logString);

      //free(cacheBuff);
      //cacheBuff = NULL;
    }
  }
  else {
    if (isChunked == 1){
      memset(request, '\0', BUFSIZE);
      int rc = transferChunks(forwardSock, connFd, request, &cacheBuff, &canCache);
      if (rc < 0) {
        //rc == -1 ? puts("fatal error in transferChunks, write to client failed") : puts("write to client succeeded but caching failed");
        // Dont save to cache if failed
        puts("Writing to client failed, not caching");//, freeing cacheBuff");
        //free(cacheBuff);
        //cacheBuff = NULL;
      }
      /*
      else if (!canCache) {
	//if (serverAllowsCaching) {
	//  puts("Writing to client succeeded but caching failed as response was too big");
	//  free(cacheBuff);
	//  cacheBuff = NULL;
	//}
        //serverAllowsCaching ? puts("Writing to client succeeded but caching failed as response was too big") : puts("Writing to client succeeded but not caching as server forbids it");
	//else puts("Writing to client succeeded but not caching as server forbids it");
        totalWrittenToClient += rc;
        //puts("Freeing cache buffer");
        //if (serverAllowsCaching) free(cacheBuff);
        //cacheBuff = NULL;
      }
      */
      else {
        //puts("Both writing of chunks to client and caching succeeded");
        totalWrittenToClient += rc;
      }
      //exitRequest(connFd, reqInf, request, servinfo);
      //return;
    }
    /*
    else {
      if (!serverAllowsCaching) {
        puts("Freeing cache buffer");
        free(cacheBuff);
        cacheBuff = NULL;
      }
      puts("Completed forwarding in single write, no need for buffering");
    }
    */
  }
  //printf("Finished servicing request with total of %d bytes written\n", totalWrittenToClient);
  printf("Finished servicing request to URI %s with a total of %d bytes written\n", reqInf->URI, totalWrittenToClient);
  // CANNOT FREE HERE, WILL SCREW UP CACHING
  //exitRequest(connFd, reqInf, request);

  if (!canCache) {
    printf("Not caching data for URI %s\n", reqInf->URI);
    puts("Freeing cacheBuffer and cacheInfo");
    free(cacheBuff);
    free(cacheInfo);
    free(cacheKey);
    free(cacheHost);
    puts("Freed cacheKey, cacheHost, cacheBuffer and cacheInfo");
  }
  //if (cacheBuff != NULL) {
  else {
    printf("Saving cached data for URI %s of length %d\n", reqInf->URI, (int)strlen(cacheBuff));
    put(cacheKey, cacheBuff, cacheHost, cacheInfo); // TEMP : Extract expiry and revalidation info from response
  }
  /*
  else {
    printf("Not caching data for URI %s\n", reqInf->URI);
    free(cacheInfo);
  }
  */
  puts("About to return from forwardRequest method");

  // Can only exitRequest after caching if necessary
  exitRequest(connFd, reqInf, request);
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
      int clientLookup;
      struct sockaddr_in addr;
      socklen_t addr_len = sizeof(addr);
      // Build UID from threadID and request number
      char *UID = malloc(30*sizeof(char));
      sprintf(UID, "%lu%d\0", threadId, requestsServiced);
      char ipstr[INET6_ADDRSTRLEN];
      bzero(ipstr, 50);
      puts("ip lookup");
      if ((clientLookup = getpeername(connFd, (struct sockaddr*)&addr, &addr_len)) < 0) {
        printf("Unable to get client ip");
      }
      inet_ntop(AF_INET, &(addr.sin_addr), ipstr, sizeof (ipstr));
      char * ipString = (char *)malloc(120 * sizeof(char));
      memset(ipString, '\0', 120*sizeof(char));
      char *time = getCurrentTimeStr();
      puts("ip lookup done");
      sprintf(ipString, "%s: %s @ %s",UID, ipstr, time);//TODO
      printf("%s\n",ipString);
      logpush(ipString);
      requestsServiced++;
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
      char * toBeFreed = stringBuffer; // value of stringBuffer will be modified by strsep, so need to remember pointer to free
      ReqInfo * parsedReq = parseRequest(stringBuffer, UID);
      cacheResult cacheRes;
      char *cachedData;
      if (parsedReq == NULL) {
        puts("Bad request detected, returning error");
        int errorWritten;
        if ((errorWritten = write(connFd, BAD_REQUEST_RESPONSE, strlen(BAD_REQUEST_RESPONSE))) < 0) {
          puts("Error returning error response");
        }
        exitRequest(connFd, parsedReq, toBeFreed);
      } else if ((cacheRes = cacheable(parsedReq, &cachedData)) == HIT) { 
        /*printf("Request %s is cacheable!\n", parsedReq->URI);*/

        char *log = malloc(50*sizeof(char));
        sprintf(log, "%s: in cache, valid", UID);
        logpush(log);
        free(log);

        int cachedWrittenToClient = (int)strlen(cachedData);
        //if (writeall(connFd, cachedData, strlen(cachedData))) {
        if (writeall(connFd, cachedData, &cachedWrittenToClient)) {
          puts("Unable to write all cached data to client");
        }
        else {
          puts("Successfully transferred cached data to client");
        }
        exitRequest(connFd, parsedReq, toBeFreed);//TEMP
      }
      else if (cacheRes == REVALIDATE) {
        char *log = malloc(50*sizeof(char));
        sprintf(log, "%s: in cache, requires validation", UID);
        logpush(log);
        free(log);

        printf("Request %s in cache but needs revalidation", parsedReq->URI);
        int revalidationResult;
        if (!(revalidationResult = getRevalidation(connFd, parsedReq, parsedReq->URI))) { // No need to revalidate
          puts("Origin server says no revalidation needed!");
          // retrieve from Cache
          int cachedWrittenToClient = (int)strlen(cachedData);
          //if (writeall(connFd, cachedData, strlen(cachedData))) {
          if (writeall(connFd, cachedData, &cachedWrittenToClient)) {
            puts("Unable to write all cached data to client");
          }
          else {
            puts("Successfully transferred cached data to client");
          }

        }
        else { // Need to revalidate - here, written to client and updates cache
          revalidationResult > 0 ? puts("New data written to client and cache updated") : puts("Error revalidating result");
          exitRequest(connFd, parsedReq, toBeFreed);
        }
      }
      else {
	  
          char *log = malloc(50*sizeof(char));
	  char * logFormatStr = "%s: not in cache\0";
	  //int logFormatStrLen = strlen(logFormatStr);
	  //int logStrLen = logFormatStrLen - 2 + strlen(UID);
	  //char log[logStrLen];
	  if (cacheRes == MISS) sprintf(log, logFormatStr, UID);
          else if (cacheRes == EXPIRED) sprintf(log, logFormatStr, UID);
          logpush(log);
          free(log);

          printf("Request %s is not cacheable because %d\n", parsedReq->URI, cacheRes);
          forwardRequest(connFd, parsedReq, toBeFreed, UID); //Blocking : Opens socket and connection to origin server, writing from that socket to client socket
	  puts("Returned from forwarding request back ot serviceRequest");
      }
      puts("Done servicing request");
    }

    else {
        printf("Thread %lu woken up, going to unlock and then try to exit\n", syscall(SYS_gettid));
        pthread_mutex_unlock(&stackMutex);
    }

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

      
      if (daemon(0, 0) < 0) {
	puts("Unable to daemonize");
	exit(1);
      }
      

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
      initlogging(); //start logger thread
      logpush("logger started..");
      printf("Main thread of id %ld about to spawn pool threads\n", syscall(SYS_gettid));
      signal(SIGINT, quit);
      //signal(SIGPIPE, socketCloseAlert);
      puts("Registered keyboard-interrupt signal-handler");
      initCache();
      puts("Initialized cache");
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
        //push(connfd, &cliaddr, &len);
	push(connfd);
	printf("Pushed connection %d onto stack\n", connfd);
      }

      close(listenfd);
      return 0;
    }
