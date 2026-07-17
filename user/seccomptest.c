#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define SECCOMP_SETMASK  0
#define SECCOMP_MAXCHILD 1

int passed = 1;

void check(int ok, char *msg) {
  if (!ok) {
    printf("FAIL: %s\n", msg);
    passed = 0;
  } else {
    printf("PASS: %s\n", msg);
  }
}

int
main(int argc, char *argv[])
{
  printf("seccomp test\n");

  // === Task 1: Syscall Allowlist ===
  printf("\n-- Task 1: Syscall Allowlist --\n");

  // Only allow: write(16), exit(2), getpid(11), sbrk(12), close(21),
  //            seccomp_ctl(23), seccomp_getlog(24)
  uint64 mask = (1UL << 2) | (1UL << 11) | (1UL << 12) | (1UL << 16)
              | (1UL << 21) | (1UL << 23) | (1UL << 24);
  seccomp_ctl(SECCOMP_SETMASK, mask);

  // Fork should be blocked (SYS_fork = 1, not in mask)
  int pid = fork();
  check(pid < 0, "fork() blocked by seccomp");

  // Open should be blocked (SYS_open = 15, not in mask)
  int fd = open("test.txt", 0);
  check(fd < 0, "open() blocked by seccomp");

  // Write should still work (SYS_write = 16, in mask)
  printf("  write() still works - good\n");

  // === Task 2: Audit Log ===
  printf("\n-- Task 2: Audit Log --\n");

  uint64 logbuf[32];
  int loglen = 32;
  seccomp_getlog(logbuf, &loglen);

  if (loglen > 0) {
    printf("  Audit log has %d blocked syscall(s):\n", loglen);
    for (int i = 0; i < loglen; i++) {
      printf("    [%d] syscall %d blocked\n", i, (int)logbuf[i]);
    }
    check(loglen >= 1, "audit log contains blocked syscalls");
  } else {
    check(0, "audit log should not be empty");
  }

  // === Task 3: Process Resource Limits ===
  printf("\n-- Task 3: Process Resource Limits --\n");

  // Reset allow mask to allow everything for fork test
  seccomp_ctl(SECCOMP_SETMASK, 0);

  // Set max children to 2
  seccomp_ctl(SECCOMP_MAXCHILD, 2);

  // First fork should succeed
  pid = fork();
  if (pid == 0) {
    exit(0);
  }
  check(pid > 0, "first fork() succeeds (within limit)");

  // Wait for first child so child_count resets
  wait(0, 0);

  // Fork two more children to test that limit works
  seccomp_ctl(SECCOMP_MAXCHILD, 2);

  pid = fork();
  if (pid == 0) {
    sleep(5);
    exit(0);
  }
  check(pid > 0, "second fork() succeeds");

  pid = fork();
  if (pid == 0) {
    sleep(5);
    exit(0);
  }
  check(pid > 0, "third fork() succeeds (child_count = 2, limit = 2)");

  // Now child_count = 2, max_children = 2, next fork should fail
  pid = fork();
  check(pid < 0, "fourth fork() blocked by child limit");

  // Wait for remaining children
  wait(0, 0);
  wait(0, 0);

  printf("\n--- %s ---\n", passed ? "seccomp test OK" : "seccomp test FAIL");

  if (passed) {
    printf("seccomp test OK\n");
  } else {
    printf("seccomp test FAIL\n");
  }
  exit(0);
}
