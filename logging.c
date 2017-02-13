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

#define LOGFILE "hi.txt"
int DEBUG = 1;
int running = 1;
pthread_mutex_t qMutex;
pthread_cond_t qCond;


typedef struct queue {
  char* message;
  int length;
  struct queue *next;
} Queue;

Queue *pendingLogs;

int queueEmpty() {
  return pendingLogs == NULL;
}

int pop(char* msg, int buffersize) {
  if (DEBUG) puts("popping");
  pthread_mutex_lock(&qMutex);
  if (queueEmpty()){
    puts("q empty");
    pthread_mutex_unlock(&qMutex);
    return 1;
  }
  else {
    puts("q NOT empty");
    Queue *currentP = pendingLogs;
    Queue *prevP = NULL;
    while (currentP->next != NULL){
      prevP = currentP;
      currentP = currentP->next;
    }
    strncpy(msg, currentP->message, currentP->length);
    msg[(currentP->length)] = '\0';
    puts("freeing currentP");
    free(currentP);
    currentP = NULL;
    if (prevP){
      prevP->next = NULL;
    } else {
      if (!currentP){
        pendingLogs = NULL;
      }
    }
    pthread_mutex_unlock(&qMutex);
    return 0;
  }
}

void *push(char* msg) {
  Queue *newQueueP = malloc(sizeof(*newQueueP));
  newQueueP->message = malloc(strlen(msg)*sizeof(char)+1);
  newQueueP->length = strlen(msg);
  strncpy(newQueueP->message, msg, strlen(msg));
  newQueueP->next = NULL;
  pthread_mutex_lock(&qMutex);
  if (queueEmpty()) {
    puts("pushing to empty");
    pendingLogs = newQueueP;
  }
  else if (!queueEmpty()){
    puts("pushing to nonempty");
    newQueueP->next = pendingLogs;
    pendingLogs = newQueueP; 
  }
  pthread_cond_signal(&qCond);
  pthread_mutex_unlock(&qMutex);
}

void leftPadZeroes(char* b, int i, int desiredLength){
  char test[20];
  sprintf(test, "%d", i);
  int difference = desiredLength - (int) strlen(test);
  /*printf("%d", difference);*/
  for (int j = 0; j < difference; j++){
    b[j] = "0";
  }
  strncpy(b+difference, test, strlen(test));
  /*for (int j = difference; j < desiredLength; j++){*/
    /*b[j] = test[j];*/
  /*}*/
  b[desiredLength] = "\0";
  printf("leftpadded: %s\n", b);
}

void *writeLog(void *intP){
  int *runningP = (int *) intP;
  FILE *logfileP = fopen(LOGFILE, "a");
  char *buff = malloc(sizeof(char)*50);   
  pthread_mutex_lock(&qMutex);
  while (*runningP){
    while (queueEmpty()){
      pthread_cond_wait(&qCond, &qMutex);
    }
    pthread_mutex_unlock(&qMutex);
    // ---- BUILD UID ----
    //uid will have first 6 digits as request num and last 6 as random num.
    int seqNum = 11;
    char uid[13];
    leftPadZeroes(uid, seqNum, 6);
    /*uid[13] = "\0";*/
    /*char *randomPortion = uid + 6;*/
    /*sprintf(uid, "%d", seqNum);*/
    /*uid[6] = "0"; //sprintf null terminates strings for us.*/
    /*sprintf(randomPortion, "%d", rand());*/
    /*printf("uid is %s\n",uid);*/
    // ---- UID BUILT ----
    int rc = pop(buff, 50);
    if (!rc) {
      printf("logging thread wrote: %s \n", buff);
      fprintf(logfileP, "%s\n", buff);
    }
    memset(buff, 0, 50*sizeof(char));
  }
  free(buff);
  puts("thread exited");
  exit(0);
}


int main(int argc, char **argv) {
  // initalize logger thread, push some stuff onto q, and let logger thread loose.
  pthread_t logThread;
  int rc;
  rc = pthread_create(&logThread, NULL, writeLog, &running);
  if (rc){
    printf("ERROR; return code from pthread_create() is %d\n", rc);
    exit(-1);
  }
  puts("created thread");
  char *b1 = malloc(20);
  pop(b1, 20);
  pop(b1, 20);
  pop(b1, 20);
  char s1[] = "hello hello hello hello\0";
  char s2[] = "from\0";
  char s3[] = "the\0";
  char s4[] = "other\0";
  char s5[] = "side\0";
  push(s1);
  push(s2);
  push(s3);
  push(s4);
  push(s5);
  printf("%d\n",pendingLogs==NULL);
  while (!queueEmpty()){
    continue;
  }
  running = 0;
  puts("done!");
}
