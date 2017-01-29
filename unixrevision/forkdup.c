#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int main() {
	int fd = open("./out", O_WRONLY | O_APPEND);
	printf("File descriptor: %d\n", fd);
	int pid;
	dup2(fd, 1);
	if (pid = fork()) {
		printf("Parent reporting child pid: %d\n", pid);
	}
	else {
		puts("Child printing hello");
	}
}
