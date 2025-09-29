#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

int main() {
  printf("Start PID=%d, parent PID=%d\n", getpid(), getppid());
  pid_t pid = fork();

  // Who am I?
  if (pid == 0) {
    printf("CHILD: PID=%d, parent PID=%d\n", getpid(), getppid());
  } else {
    printf("PARENT: PID=%d, child PID=%d\n", getpid(), pid);
  }
  return 0;
}
