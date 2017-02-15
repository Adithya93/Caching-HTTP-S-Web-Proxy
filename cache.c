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

// Implements a cache as a map of key (URI) to value (struct nodes). 
// Hashmap handles collisions by appending nodes to the end of linked lists in each bucket.

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

pthread_mutex_t cacheLock;


void freeNode(NODE * old) {
  free(old->key);
  free(old->val);
  free(old->info->eTag);
  free(old->host);
  free(old->info->expiryTime);
  free(old->info);
  free(old);
}

NODE * makeNode(char * key, char * val, char * host, CacheInfo * cacheInfo) {
  NODE * newNode = (NODE *)malloc(sizeof(NODE));
  printf("Allocated node %p\n", newNode);
  newNode->key = key;
  newNode->val = val;
  newNode->host = host;
  newNode->info = cacheInfo;
  newNode->next = NULL;
  return newNode;
}

void updateNode(NODE * node, char * value, CacheInfo * cacheInfo) {
  free(node->val);
  node->val = value;
  free(node->info);
  node->info = cacheInfo;
}

NODE * getNode(int bucket, char * key) {
  NODE * current = MAP[bucket];
  while (current != NULL) {
    if (strcmp(current->key, key) == 0) {
      printf("Found key %s, value is %s\n", key, current->val);
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
  else {
    printf("ID found and deemed valid");
    result = foundNode->val;
  }
  pthread_mutex_unlock(&cacheLock);
  return result;
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

NODE * get(char * key) {
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
