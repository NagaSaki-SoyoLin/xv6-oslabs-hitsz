#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

extern struct proc proc[NPROC];  // 全局进程表, 定义在 kernel/proc.c

uint64 sys_exit(void) {
  int n;
  if (argint(0, &n) < 0) return -1;
  exit(n);
  return 0;  // not reached
}

uint64 sys_getpid(void) { return myproc()->pid; }

uint64 sys_fork(void) { return fork(); }

uint64 sys_wait(void) {
  uint64 p;
  int flags; // 新增的第二个参数, 非阻塞选项
  if (argaddr(0, &p) < 0) return -1;
  if (argint(1, &flags) < 0) return -1; // 从 trapframe->a1 取出 flags
  return wait(p, flags); // 传入 p 和 flags
}

uint64 sys_sbrk(void) {
  int addr;
  int n;

  if (argint(0, &n) < 0) return -1;
  addr = myproc()->sz;
  if (growproc(n) < 0) return -1;
  return addr;
}

uint64 sys_sleep(void) {
  int n;
  uint ticks0;

  if (argint(0, &n) < 0) return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (myproc()->killed) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64 sys_kill(void) {
  int pid;

  if (argint(0, &pid) < 0) return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void) {
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64 sys_rename(void) {
  char name[16];
  int len = argstr(0, name, MAXPATH);
  if (len < 0) {
    return -1;
  }
  struct proc *p = myproc();
  memmove(p->name, name, len);
  p->name[len] = '\0';
  return 0;
}

// 新增的系统调用 yield
uint64 sys_yield(void) {
  struct proc *p = myproc(); // 获取当前进程

  acquire(&p->lock); // 尝试获取当前进程的锁
  printf("Save the context of the process to the memory region from address %p to %p\n",
    &p->context, &p->context + 1); // 打印保存当前进程的内核线程上下文的地址范围
  printf("Current running process pid is %d and user pc is %p\n",
    p->pid, p->trapframe->epc); // 打印当前正在运行进程的 pid 以及它陷入内核前的用户态 pc
  release(&p->lock); // 释放当前进程的锁

  for (struct proc *pn = proc; pn < &proc[NPROC]; pn++) {
    // pn 下一个可能被调度运行的进程
    acquire(&pn->lock); // 尝试获取 pn 的锁
    if (pn->state == RUNNABLE) {
      printf("Next runnable process pid is %d and user pc is %p\n",
        pn->pid, pn->trapframe->epc); // 打印下一个可能被调度运行进程的 pid 以及它对应的用户态 pc
      release(&pn->lock); // 释放 pn 的锁
      break;
    }
    release(&pn->lock); // 释放 pn 的锁
  }

  yield(); // 调用 yield 函数
  return 0;
}
