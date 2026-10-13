//
// Created by 18256 on 2026/9/14.
//
#include "kernel/types.h"
#include "user.h"
int main(int argc, char *argv[]) {
  if (argc != 1) {
    printf("pingpong needs no argument!\n");
    exit(1);
  }

  int pfc[2], pcf[2];  // 两个管道, pfc为父进程到子进程, pcf为子进程到父进程
  pipe(pfc);           // father to child
  pipe(pcf);           // child to father

  int f_pid = getpid(), pid = fork();  // 获取父进程id, 创建子进程
  if (pid == 0) {
    close(pcf[0]);  // 关闭子进程到父进程的读端
    close(pfc[1]);  // 关闭父进程到子进程的写端

    char buf[5];  // 缓冲区
    read(pfc[0], buf, 5);
    close(pfc[0]);  // 关闭父进程到子进程的读端
    printf("%d: received %s from pid %d\n", getpid(), buf, f_pid);

    write(pcf[1], "pong", 5);
    close(pcf[1]);  // 关闭子进程到父进程的写端

    exit(0);
  } else if (pid > 0) {
    close(pfc[0]);  // 关闭父进程到子进程的读端
    close(pcf[1]);  // 关闭子进程到父进程的写端

    write(pfc[1], "ping", 5);
    close(pfc[1]);  // 关闭父进程到子进程的写端
    wait(0);        // 等待子进程结束

    char buf[5];  // 缓冲区
    read(pcf[0], buf, 5);
    close(pcf[0]);  // 关闭子进程到父进程的读端
    printf("%d: received %s from pid %d\n", getpid(), buf, pid);

    exit(0);
  } else {
    printf("fork error\n");  // 创建子进程失败
    exit(1);
  }
}