#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static char *
base_name(char *path)
{
  char *p;

  p = path + strlen(path);

  while (p > path && p[-1] != '/')
    p--;

  return p;
}

static int
is_dot(char *name)
{
  return strcmp(name, ".") == 0 ||
         strcmp(name, "..") == 0;
}

static void
find_file(char *path, char *filename)
{
  char buf[512];
  char *p;
  int fd;
  uint length;
  uint needs_slash;
  struct dirent entry;
  struct stat st;

  fd = open(path, O_RDONLY);
  if (fd < 0)
  {
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if (fstat(fd, &st) < 0)
  {
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch (st.type)
  {
  case T_FILE:
    if (strcmp(base_name(path), filename) == 0)
      printf("%s\n", path);
    break;

  case T_DIR:
    length = strlen(path);
    needs_slash = length == 0 || path[length - 1] != '/';

    if (length + needs_slash + DIRSIZ + 1 > sizeof(buf))
    {
      fprintf(2, "find: path too long\n");
      break;
    }

    strcpy(buf, path);
    p = buf + length;

    if (needs_slash)
      *p++ = '/';

    while (read(fd, &entry, sizeof(entry)) == sizeof(entry))
    {
      if (entry.inum == 0)
        continue;

      memmove(p, entry.name, DIRSIZ);
      p[DIRSIZ] = '\0';

      if (is_dot(p))
        continue;

      find_file(buf, filename);
    }

    break;
  }

  close(fd);
}

int main(int argc, char *argv[])
{
  if (argc != 3)
  {
    fprintf(2, "usage: find path filename\n");
    exit(1);
  }

  find_file(argv[1], argv[2]);

  exit(0);
}