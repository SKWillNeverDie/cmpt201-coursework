#include <stdio.h>
#include <unistd.h>

int main() {
  fork();
  for (int i = 0; i < 40; i++) {
    printf("Sleeping%d\n", i);
    sleep(1);
  }
  printf("DONE\n");
}
