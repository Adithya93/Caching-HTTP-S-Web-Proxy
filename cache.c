#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <syscall.h>
#include <time.h>

/* A simple cache */
int NUM_BUCKETS = 100;
char DELIM[2] = " \0";

typedef enum {MISS, HIT, EXPIRED, REVALIDATE} cacheResult;


typedef struct cacheinfo {
  int needsRevalidation;
  struct tm * expiryTime;
  char *eTag;
} CacheInfo;


typedef struct node {
  char *key;
  char *val;
  char *host;
  CacheInfo * info;
  struct node * next;
} NODE;

typedef struct fields {
  int isGet;
  char key[256];
  char val[256];
} FIELDS;

NODE* MAP[100];
char buffer[256];

//pthread_mutex_t bufferLock;
pthread_mutex_t cacheLock;

//long currentTime = 0;// TEMP

void freeNode(NODE * old) {
  free(old->key);
  free(old->val);
  free(old->info->eTag);
  free(old->host);
  free(old->info->expiryTime);
  free(old->info);
  free(old);
}
/*
void freeFields(FIELDS * f) {
  free(f->key);
  free(f->val);
  free(f);
}
*/

NODE * makeNode(char * key, char * val, char * host, CacheInfo * cacheInfo) {
  NODE * newNode = (NODE *)malloc(sizeof(NODE));
  //newNode->key = (char *)malloc((strlen(key) + 1) * sizeof(char));
  //strcpy(newNode->key, key);
  printf("Allocated node %p\n", newNode);
  newNode->key = key;
  //newNode->val = (char *)malloc((strlen(val) + 1) * sizeof(char));
  //strcpy(newNode->val, val);
  newNode->val = val;
  newNode->host = host;
  //newNode->expiry = expiry;
  //newNode->needsRevalidation = needsRevalidation;
  newNode->info = cacheInfo;
  newNode->next = NULL;
  return newNode;
}

/*
NODE * setCacheControl(NODE * )
*/

void updateNode(NODE * node, char * value, CacheInfo * cacheInfo) {
  free(node->val);
  node->val = value;
  //node->expiry = expiry;
  //node->needsRevalidation = needsRevalidation;
  free(node->info);
  node->info = cacheInfo;
}

NODE * getNode(int bucket, char * key) {
  NODE * current = MAP[bucket];
  while (current != NULL) {
    if (strcmp(current->key, key) == 0) {
      printf("Found key %s, value is %s\n", key, current->val);
      //puts("Returning node");
      return current;
    }
    current = current->next;
  }
  printf("Key %s not found\n", key);
  return NULL;
}


char * getValue(int bucket, char * key) {
  pthread_mutex_lock(&cacheLock);
  NODE * foundNode = getNode(bucket, key);
  char * result;
  puts("Testing validity in getValue method");
  printf("Address of found node %p\n", foundNode);
  if ((void*)foundNode == NULL) {
    printf("ID %s not in cache\n", key);
    //return NULL;
    result = NULL;
  }

  /*
  else if (foundNode->expiry < currentTime) {
    printf("ID %s found but expired\n", key);
    //return NULL;
    result = NULL;
  }
  */
  /*
  else if (foundNode->needsRevalidation) {
    printf("ID %s found but needs revalidation\n", key);
    //return NULL;
    result = NULL;
  }
  */

  else {
    printf("ID found and deemed valid");
    result = foundNode->val;
  }
  pthread_mutex_unlock(&cacheLock);
  return result;
  //return foundNode->val;
}

void putKey(int bucket, char * key, char * value, char * host, CacheInfo * cacheInfo) {
  pthread_mutex_lock(&cacheLock);
  NODE * targetNode = getNode(bucket, key);
  if (targetNode == NULL) {
    targetNode = makeNode(key, value, host, cacheInfo);
    targetNode->next = MAP[bucket];
    MAP[bucket] = targetNode;
    printf("Added new key %s to cache with value %s, cacheInfo ptr %p\n", key, value, cacheInfo);
  }
  else {
    updateNode(targetNode, value, cacheInfo);
    printf("Updated value as %s, cacheInfo ptr of %p for key %s in cache\n", value, cacheInfo, key);
  }
  pthread_mutex_unlock(&cacheLock);
}

int indexOf(char * key) { // Simple but weak hash function
  int hashVal = 0;
  char * current = key;
  while (*current != '\0') {
    hashVal += (int)(*current);
    current ++;
  }
  int index = hashVal % NUM_BUCKETS;
  printf("Index of key %s is %d\n", key, index);
  return index;
}

//char * get(char * key) {
NODE * get(char * key) {
//return getValue(indexOf(key), key);
  return getNode(indexOf(key), key);
}

void put(char * key, char * value, char * host, CacheInfo * cacheInfo) {
  return putKey(indexOf(key), key, value, host, cacheInfo);
}


void clearCache() {
  NODE * node;
  NODE * temp;
  for (int bucket = 0; bucket < NUM_BUCKETS; bucket ++) {
    node = MAP[bucket];
    while (node != NULL) {
      temp = node->next;
      freeNode(node);
      node = temp;
    }
  }
}






/*
void parseSentence(FIELDS * f, char * sentence) {
  char * getStatus = strtok(sentence, DELIM);
  if (getStatus == NULL) {
    perror("Insufficient fields");
    exit(1);
  }
  f->isGet = strcmp(getStatus, "p");
  printf("Is it a get request? %d\n", f->isGet);
  getStatus = strtok(NULL, DELIM);
  if (getStatus == NULL) {
    perror("Insufficient fields");
    exit(1);
  }
  printf("Key : %s\n", getStatus);
  strcpy(f->key, getStatus);
  if (!f->isGet) {
    getStatus = strtok(NULL, DELIM);
    if (getStatus == NULL) {
      perror("Insufficient fields");
      exit(1);
    }
    printf("Value : %s\n", getStatus);
    strcpy(f->val, getStatus);
  }
  return;
}


void copyRequest(char * dest) {
  pthread_mutex_lock(&bufferLock);
  strcpy(dest, buffer);
  pthread_mutex_unlock(&bufferLock);
}


void * serveRequest() {
  printf("Thread %lu starting.\n", syscall(SYS_gettid));
  char sentence[256];
  char * result;
  copyRequest(sentence);
  FIELDS * f = (FIELDS *)malloc(sizeof(FIELDS));
  *(sentence + strlen(sentence) - 1) = '\0';
  parseSentence(f, sentence);
  if (f->isGet) {
    result = get(f->key);
    printf("Value of key %s : %s\n", f->key, result);
  }
  else {
    put(f->key, f->val, 100, 0);
  }
  free(f);
  printf("Thread %lu exiting.\n", syscall(SYS_gettid));
}
*/

int initCache() {
  return pthread_mutex_init(&cacheLock, NULL);// || pthread_mutext_init(&bufferLock, NULL);
}

/*
   int main() {
   puts("Server starting.");
   pthread_mutex_init(&cacheLock, NULL);
   pthread_mutex_init(&bufferLock, NULL);
   while (fgets(buffer, 256, stdin) != NULL) {
   pthread_t childThread;
   pthread_create(&childThread, NULL, serveRequest, NULL);
// Call join with no hang (asynchronous wait) to log child's exit status
}
puts("Server exiting.");
clearCache();
exit(0);
}

*/
