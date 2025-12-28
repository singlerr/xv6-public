#include "types.h"
#include "user.h"

#define OPT_A (1)
#define OPT_P (1 << 1)

// same value with PFFNUM
#define MAX_FRINFO 60000

// print usage
static void
usage(void)
{
    printf(1, "usage: memdump [-a] [-p PID]\n");
    exit();
}

int main(int argc, char *argv[])
{
    char opt;
    int opts = 0;
    int pid;

    if (argc == 1)
        usage();

    pid = getpid();

    while ((opt = (char)getopts(argc, argv, "ap:")) != -1)
    {
        switch (opt)
        {
        case 'a':
            opts |= OPT_A;
            break;
        case 'p':
            opts |= OPT_P;
            pid = atoi(argv[optind]);
            break;
        default:
            break;
        }
    }
    static struct physframe_info buf[MAX_FRINFO];
    int n = dump_physmem_info((void *)buf, MAX_FRINFO);
    if (n < 0)
    {
        printf(1, "memdump: dump_physmem_info failed\n");
        exit();
    }

    printf(1, "[memdump] pid=%d\n", getpid());
    printf(1, "[frame#]\t[alloc]\t[pid]\t[start_tick]\n");

    for (int i = 0; i < n; i++)
    {
        // if -p option enabled, it precedes over option -a
        // which means if -p, -a both enabled, it will print entries only with same pid
        if ((opts & OPT_P) && buf[i].pid != pid)
        {
            continue;
        }

        if ((!(opts & OPT_A)) && buf[i].allocated == 0)
        {
            continue;
        }
        // print entry
        printf(1, "%d\t\t%d\t%d\t%d\n",
               buf[i].frame_index, buf[i].allocated,
               buf[i].pid, buf[i].start_tick);
    }
    exit();
}