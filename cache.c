#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <syscall.h>

/* A simple cache */
int NUM_BUCKETS = 100;
char DELIM[2] = " \0";

typedef struct node {
	char * key;
	char * val;
	long expiry; // Replace with ctime struct in future?
	int needsRevalidation;
	struct node * next;
} NODE;

typedef struct fields {
	int isGet;
	char key[256];
	char val[256];	
} FIELDS;

NODE* MAP[100];
char buffer[256];

pthread_mutex_t bufferLock;
pthread_mutex_t cacheLock;

long currentTime = 0;// TEMP

void freeNode(NODE * old) {
	free(old->key);
	free(old->val);
	free(old);
}

void freeFields(FIELDS * f) {
	free(f->key);
	free(f->val);
	free(f);
}

NODE * makeNode(char * key, char * val, long expiry, int needsRevalidation) {
	NODE * newNode = (NODE *)malloc(sizeof(NODE));
	newNode->key = (char *)malloc((strlen(key) + 1) * sizeof(char));
	strcpy(newNode->key, key);
	newNode->val = (char *)malloc((strlen(val) + 1) * sizeof(char));
	strcpy(newNode->val, val);
	newNode->expiry = expiry;
	newNode->needsRevalidation = needsRevalidation;
	newNode->next = NULL;
	return newNode;
}

void updateNode(NODE * node, char * value, long expiry, int needsRevalidation) {
	free(node->val);
	node->val = value;
	node->expiry = expiry;
	node->needsRevalidation = needsRevalidation;
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
	if (foundNode == NULL) {
		printf("ID %s not in cache\n", key);
		//return NULL;
		result = NULL;	
	}
	else if (foundNode->expiry < currentTime) {
		printf("ID %s found but expired\n", key);
		//return NULL;
		result = NULL;
	}
	else if (foundNode->needsRevalidation) {
		printf("ID %s found but needs revalidation\n", key);
		//return NULL;
		result = NULL;
	}
	else {
		result = foundNode->val;
	}
	pthread_mutex_unlock(&cacheLock);
	return result;
	//return foundNode->val;
}

void putKey(int bucket, char * key, char * value, long expiry, int needsRevalidation) {
	pthread_mutex_lock(&cacheLock);
	NODE * targetNode = getNode(bucket, key);
	if (targetNode == NULL) {
		targetNode = makeNode(key, value, expiry, needsRevalidation);
		targetNode->next = MAP[bucket];
		MAP[bucket] = targetNode;
		printf("Added new key %s to cache with value %s, expiry %lu and revalidation status %d\n", key, value, expiry, needsRevalidation);
	}
	else {
		updateNode(targetNode, value, expiry, needsRevalidation);
		printf("Updated value as %s, expiry as %lu and revalidation status as %d of key %s in cache\n", value, expiry, needsRevalidation, key);
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

char * get(char * key) {
	return getValue(indexOf(key), key);
}

void put(char * key, char * value, long expiry, int needsRevalidation) {
	return putKey(indexOf(key), key, value, expiry, needsRevalidation);
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
	printf("Thread %d starting.\n", syscall(SYS_gettid));
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
	printf("Thread %d exiting.\n", syscall(SYS_gettid));
}


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

