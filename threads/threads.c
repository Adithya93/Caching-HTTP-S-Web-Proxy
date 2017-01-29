#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>

void * result;
int threadArg;

void* hello() {
	puts("Hello from child thread!");
	int * resultStore = (int *) malloc(sizeof(int));
	*resultStore = 5;
	return (void*) resultStore;
}

int doFib(int num);

void * fib(void * num) {
	int* numPtr = (int*) num;
	int result = doFib(*numPtr);
	int* resultPtr = (int*) malloc(sizeof(int));
	*resultPtr = result;
	puts("Ian is a nerdbeast");
	return (void*) resultPtr;
}

// Deliberately use inefficient fib to simulate computationally intensive process
int doFib(int num) {
	if (num <= 0) return 0;
	if (num == 1) return 1;
	return doFib(num - 1) + doFib(num - 2);	
}



int main(int argc, char * argv[]) {
	if (argc < 2) {
		printf("Usage: %s <num>\n", argv[0]);
		exit(1);
	}
	threadArg = atoi(argv[1]);
	pthread_t* childThread = (pthread_t*) malloc(sizeof(pthread_t));
	result = (int*) malloc(sizeof(int));
	
	if (pthread_create(childThread, NULL, fib, (void *)&threadArg)) {
		puts("Unable to spawn thread.");
		exit(1);
	}
	puts("Main thread waiting for child thread to complete.");
	int exitStatus = pthread_join(*childThread, &result);
	printf("Exit status of child thread is %d and return value for fib %d is %d\n", exitStatus, threadArg, *((int*)result));
	free(result);
}




