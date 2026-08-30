#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

#define MAXLINE 512

static void
run_line(char *line, int length, int argc, char *argv[])
{
    char *args[MAXARG];
    int arg_count;
    int position;
    int i;
    int pid;

    line[length] = '\0';
    arg_count = 0;

    /*
     * 复制 xargs 后面原有的命令和固定参数。
     * argv[0] 是 xargs，因此从 argv[1] 开始复制。
     */
    for (i = 1; i < argc; i++)
    {
        if (arg_count >= MAXARG - 1)
        {
            fprintf(2, "xargs: too many initial arguments\n");
            exit(1);
        }

        args[arg_count++] = argv[i];
    }

    /*
     * 将输入行按照空格和制表符拆分成参数。
     */
    position = 0;

    while (position < length)
    {
        while (position < length &&
               (line[position] == ' ' || line[position] == '\t'))
        {
            position++;
        }

        if (position >= length)
            break;

        if (arg_count >= MAXARG - 1)
        {
            fprintf(2, "xargs: too many arguments\n");
            exit(1);
        }

        args[arg_count++] = &line[position];

        while (position < length &&
               line[position] != ' ' &&
               line[position] != '\t')
        {
            position++;
        }

        if (position < length)
            line[position++] = '\0';
    }

    /*
     * 输入为空行时不执行命令。
     */
    if (arg_count == argc - 1)
        return;

    args[arg_count] = 0;

    pid = fork();

    if (pid < 0)
    {
        fprintf(2, "xargs: fork failed\n");
        exit(1);
    }

    if (pid == 0)
    {
        exec(args[0], args);
        fprintf(2, "xargs: exec %s failed\n", args[0]);
        exit(1);
    }

    wait(0);
}

int main(int argc, char *argv[])
{
    char line[MAXLINE];
    char c;
    int length;
    int result;

    if (argc < 2)
    {
        fprintf(2, "usage: xargs command [arguments ...]\n");
        exit(1);
    }

    /*
     * 至少为输入参数和结尾的空指针各保留一个位置。
     */
    if (argc >= MAXARG)
    {
        fprintf(2, "xargs: too many initial arguments\n");
        exit(1);
    }

    length = 0;

    while ((result = read(0, &c, 1)) == 1)
    {
        if (c == '\n')
        {
            run_line(line, length, argc, argv);
            length = 0;
            continue;
        }

        if (length >= MAXLINE - 1)
        {
            fprintf(2, "xargs: input line too long\n");
            exit(1);
        }

        line[length++] = c;
    }

    if (result < 0)
    {
        fprintf(2, "xargs: read failed\n");
        exit(1);
    }

    /*
     * 处理末尾没有换行符的最后一行。
     */
    if (length > 0)
        run_line(line, length, argc, argv);

    exit(0);
}