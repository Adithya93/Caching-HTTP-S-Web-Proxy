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

void updateNode(NODE * node, char * key, char * value, char * host, CacheInfo * cacheInfo) {
  printf("Updating cache node %p\n", node);
  free(node->val);
  node->val = value;
  if (node->info != cacheInfo) { //in the case of handling an expired cache entry, cacheInfo is more up-to-date than node->info, with correct expiry time, eTag, etc
    printf("Detected change of cacheInfo in updateNode method, original cacheInfo ptr %p, new cacheInfo ptr %p\n", node->info, cacheInfo);
    free(node->info);
    node->info = cacheInfo;
    //printf("New eTag: %s\n", node->info->eTag);
  }
  else { // in the case of handling revalidation, same cacheInfo ptr is passed in
    printf("New eTag: %s\n", node->info->eTag);
  }
  if (node->key != key) {
    printf("Freeing comparison key %s\n", key);
    free(key);
  }
  if (node->host != host) {
    printf("Freeing host ptr %s passed in to updateNode\n", host);
    free(host);
  }
  puts("Done updating node");
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
    updateNode(targetNode, key, value, host, cacheInfo);
    printf("Updated value as %s, cacheInfo ptr of %p for key %s in cache\n", value, cacheInfo, cacheInfo->key);
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

