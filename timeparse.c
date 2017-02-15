#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _XOPEN_SOURCE
#include <time.h>

enum s {DAY, DDAY, MONTH, YEAR, HR, MIN, SEC, TIMEZONE};
// parse time of the format Expires: Thu, 01 Jan 1970 00:20:55 GMT

struct tm *parseTime(char *timeStr){
  struct tm* tmm = malloc(sizeof(struct tm));
  strptime(timeStr, "%a, %d %b %Y %H:%M:%S %Z", tmm);
  return tmm;
}

void printTime(struct tm *tmm){
  printf("wday %d\n", tmm->tm_wday);
  printf("mday %d\n", tmm->tm_mday);
  printf("mon %d\n", tmm->tm_mon);
  printf("yr %d\n", tmm->tm_year);
  printf("hr %d\n", tmm->tm_hour);
  printf("min %d\n", tmm->tm_min);
  printf("sec %d\n", tmm->tm_sec);
}


int timeAgtB(struct tm *tmm, struct tm *tmm2){
  // return 1 if gt, -1 if sm, 0 if same
  time_t time1_t = mktime(tmm);
  time_t time2_t = mktime(tmm2);
  if (difftime(time1_t,time2_t) == (double) 0) return 0;
  else {
    return difftime(time1_t,time2_t) > (double) 0 ? 1: -1;
  }
}

struct tm *getCurrentTime(){
  struct tm *ret = malloc(sizeof(struct tm));
  time_t currTimeT = time(NULL);
  memcpy (ret, gmtime(&currTimeT), sizeof (struct tm));
  return ret;
}

struct tm *getTimeFromExpiry(long expiry){
  struct tm *ret = malloc(sizeof(struct tm));
  time_t currTimeT = time(NULL);
  time_t newTimeT = currTimeT + expiry;
  struct tm* test2 = gmtime(&newTimeT);
  memcpy (ret, gmtime(&newTimeT), sizeof (struct tm));
  return ret;
}

/*int    tm_sec   seconds [0,61]*/
/*int    tm_min   minutes [0,59]*/
/*int    tm_hour  hour [0,23]*/
/*int    tm_mday  day of month [1,31]*/
/*int    tm_mon   month of year [0,11]*/
/*int    tm_year  years since 1900*/
/*int    tm_wday  day of week [0,6] (Sunday = 0)*/
/*int    tm_yday  day of year [0,365]*/
/*int    tm_isdst daylight savings flag*/

/*int main (int argc, char **argv){*/
  /*char *time = "Thu, 03 Jan 1970 00:20:55 GMT";*/
  /*struct tm *tmm = parseTime(time);*/
  /*char *time2 = "Thu, 01 Jan 1970 00:20:55 GMT";*/
  /*struct tm *tmm2 = parseTime(time2);*/
  /*time_t time1_t = mktime(tmm);*/
  /*time_t time2_t = mktime(tmm2);*/
  /*long expiry = 31536000;*/
  /*struct tm *test = getTimeFromExpiry(expiry);*/
  /*struct tm *testcurrenttime = getCurrentTime();*/
  /*printTime(test);*/
  /*puts("----");*/
  /*printTime(testcurrenttime);*/
  /*int a = timeAgtB(tmm, tmm2); //should return 1*/
  /*int b = timeAgtB(tmm2, tmm); //should return -1*/
  /*int c = timeAgtB(tmm, tmm); //should return 0*/
  /*printf("%d %d %d",a,b,c);*/
/*}*/
