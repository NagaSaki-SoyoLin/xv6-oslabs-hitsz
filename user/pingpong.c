//
// Created by 18256 on 2026/9/14.
//
#include "kernel/types.h"
#include "user.h"
int main(int argc, char *argv[]) {
  int pfc[2], pcf[2];
  pipe(pfc);
  pipe(pcf);

  int fpid = getpid(), pid = fork();
  if (pid > 0) {
    close(pcf[0]);
    close(pfc[1]);

    char buf[5];
    read(pfc[0], buf, 5);
    close(pfc[0]);
    printf("%d: received %s from pid %d\n", getpid(), buf, fpid);

    write(pcf[1], "pong", 5);
    close(pcf[1]);
    exit(0);
  } else if (pid > 0) {
    close(pfc[0]);
    close(pcf[1]);

    write(pfc[1], "ping", 5);
    close(pfc[1]);
    wait(0);

    char buf[5];
    read(pcf[0], buf, 5);
    close(pcf[0]);
    printf("%d: received %s from pid %d\n", getpid(), buf, pid);
    exit(0);
  } else {
    printf("fork error\n");
    exit(1);
  }
}