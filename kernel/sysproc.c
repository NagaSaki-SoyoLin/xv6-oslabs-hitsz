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

  for (int i = 0; i < NPROC; i++) {
    struct proc *pn = &proc[(p - proc + i + 1) % NPROC]; // pn 下一个可能被调度运行的进程
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

// 新增的系统调用 seccomp_ctl
uint64 sys_seccomp_ctl(void) {
  int op; // 操作码, op=0时设置系统调用白名单位掩码, op=1时设置最大子进程数
  uint64 arg; // op=0时arg为位掩码, op=1时为arg最大数量
  if (argint(0, &op) < 0) return -1; // 从 trapframe->a0 取出 op
  if (argaddr(1, &arg) < 0) return -1; // 从 trapframe->a1 取出 arg

  struct proc *p = myproc(); // 获取当前进程
  if (op == 0) {
    p->seccomp_mask = arg; // 设置系统调用白名单掩码
    return 0; // 设置成功
  }
  if (op == 1) {
    p->maxchcnt = arg; // 设置最大子进程数
    return 0; // 设置成功
  }
  return -1; // 不支持的操作码
}

// 新增的系统调用 get_seccomp_getlog
uint64 sys_seccomp_getlog(void) {
  uint64 buf_addr, len_addr; // 函数参数传入的是用户态指针(地址)
  if (argaddr(0, &buf_addr) < 0) return -1; // 从 trapframe->a0 取出 buf_addr
  if (argaddr(1, &len_addr) < 0) return -1; // 从 trapframe->a1 取出 len_addr

  struct proc *p = myproc(); // 获取当前进程
  int len; // 用户传入的缓冲区容量
  int n; // 实际能写入的条数

  // 用户到内核: 读入用户给的缓冲区容量 len
  if (copyin(p->pagetable, (char *)&len, len_addr, sizeof(len)) < 0) return -1;
  // 实际条数 = min(用户容量, 已记录条数)
  n = p->auditlog_pos < len ? p->auditlog_pos : len;
  // 内核到用户: 把前 n 条日志写到用户缓冲区
  if (n > 0 && copyout(p->pagetable, buf_addr, (char *)p->auditlog, n * sizeof(uint64)) < 0) return -1;
  // 内核到用户: 把实际写入条数写回用户的 len
  if (copyout(p->pagetable, len_addr, (char *)&n, sizeof(n)) < 0) return -1;

  return 0;
}
