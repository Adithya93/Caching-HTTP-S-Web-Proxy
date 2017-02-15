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

#define LOGFILE "/var/log/erss-proxy.log"

pthread_mutex_t qMutex;
pthread_cond_t qCond;
int loggerrunning = 1;
typedef struct queue {
  char* message;
  int length;
  struct queue *next;
} Queue;

Queue *pendingLogs;

int queueEmpty() {
  return pendingLogs == NULL;
}

int logpop(char* msg, int buffersize) {
  pthread_mutex_lock(&qMutex);
  if (queueEmpty()){
    pthread_mutex_unlock(&qMutex);
    return 1;
  }
  else {
    Queue *currentP = pendingLogs;
    Queue *prevP = NULL;
    while (currentP->next != NULL){
      prevP = currentP;
      currentP = currentP->next;
    }
    strncpy(msg, currentP->message, currentP->length);
    msg[(currentP->length)] = '\0';
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

void *logpush(char* msg) {
  Queue *newQueueP = malloc(sizeof(*newQueueP));
  newQueueP->message = malloc(strlen(msg)*sizeof(char)+1);
  newQueueP->length = strlen(msg);
  strncpy(newQueueP->message, msg, strlen(msg));
  newQueueP->next = NULL;
  pthread_mutex_lock(&qMutex);
  if (queueEmpty()) {
    pendingLogs = newQueueP;
  }
  else if (!queueEmpty()){
    newQueueP->next = pendingLogs;
    pendingLogs = newQueueP; 
  }
  pthread_cond_signal(&qCond);
  pthread_mutex_unlock(&qMutex);
}


void *writeLog(void *intP){
  // called by logger thread to write logfiles by popping log requests off a queue. 
  int *runningP = (int *) intP;
  FILE *logfileP = fopen(LOGFILE, "a+");
  printf("writing to %S", LOGFILE);
  char *buff = malloc(sizeof(char)*50);   
  pthread_mutex_lock(&qMutex);
  while (*runningP){
    while (queueEmpty()){
      pthread_cond_wait(&qCond, &qMutex);
    }
    pthread_mutex_unlock(&qMutex);
    int rc = logpop(buff, 50);
    if (!rc) {
      printf("logging thread wrote: %s \n", buff);
      fprintf(logfileP, "%s\n", buff);
    }
    memset(buff, 0, 50*sizeof(char));
  }
  fclose(logfileP);
  free(buff);
  exit(0);
}

int initlogging(){
  // initalize logger thread, push some stuff onto q, and let logger thread loose.
  pthread_t logThread;
  int rc;
  rc = pthread_create(&logThread, NULL, writeLog, &loggerrunning);
  if (rc){
    printf("ERROR; return code from pthread_create() is %d\n", rc);
    return -1;
  }
  return 0;
}

int stoplogging(){
  loggerrunning = 0;
}

/*int main(int argc, char **argv) {*/
  /*// initalize logger thread, push some stuff onto q, and let logger thread loose.*/
  /*pthread_t logThread;*/
  /*int rc;*/
  /*rc = pthread_create(&logThread, NULL, writeLog, &running);*/
  /*if (rc){*/
    /*printf("ERROR; return code from pthread_create() is %d\n", rc);*/
    /*exit(-1);*/
  /*}*/
  /*puts("created thread");*/
  /*char *b1 = malloc(20);*/
  /*logpop(b1, 20);*/
  /*logpop(b1, 20);*/
  /*logpop(b1, 20);*/
  /*char s1[] = "hello hello hello hello\0";*/
  /*char s2[] = "from\0";*/
  /*char s3[] = "the\0";*/
  /*char s4[] = "other\0";*/
  /*char s5[] = "side\0";*/
  /*logpush(s1);*/
  /*logpush(s2);*/
  /*logpush(s3);*/
  /*logpush(s4);*/
  /*logpush(s5);*/
  /*printf("%d\n",pendingLogs==NULL);*/
  /*while (!queueEmpty()){*/
    /*continue;*/
  /*}*/
  /*running = 0;*/
  /*puts("done!");*/
/*}*/
