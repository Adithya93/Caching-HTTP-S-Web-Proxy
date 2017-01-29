#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int main() {

	printf("Process id of main: %d\n", getpid());
	int pid;
	if (pid = fork()) { // parent	
		printf("Child pid: %d\n", pid);
		puts("Parent exiting.");
		exit(0);
	}
	printf("Child of pid %d exiting\n", getpid());
	exit(0);
}
