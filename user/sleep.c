// user/sleep.c
#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
  if (argc != 2)
  {
    fprintf(2, "usage: sleep ticks\n");
    exit(1);
  }

  int deltatime = atoi(argv[1]);
  sleep(deltatime);
  exit(0);
}
