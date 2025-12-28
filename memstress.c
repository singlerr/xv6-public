#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

// usage
static void
usage(void)
{
    printf(1, "usage: memstress [-n pages] [-t ticks] [-w]\n");
    exit();
}

#define OPT_N 1
#define OPT_T (1 << 1)
#define OPT_W (1 << 2)

// same with PGSIZE
#define PGSIZE 4096

int main(int argc, char *argv[])
{
    char c;
    int opts = 0;
    char *addr;
    int pages = 10;
    int hold_ticks = 200;
    int do_write;
    char *base;
    if (argc == 1)
        usage();

    while ((c = (char)getopts(argc, argv, "n:t:w")) != -1)
    {
        switch (c)
        {
        case 'n':
            opts |= OPT_N;
            pages = atoi(argv[optind]);
            break;
        case 't':
            opts |= OPT_T;
            hold_ticks = atoi(argv[optind]);
            break;
        case 'w':
            opts |= OPT_W;
            break;
        default:
            break;
        }
    }

    do_write = opts & OPT_W ? 1 : 0;
    int pid = getpid();
    printf(1, "[memstress] pid=%d pages=%d hold=%d ticks write=%d\n", pid, pages, hold_ticks, do_write);

    // requests kernel to allocate numpages of virtual pages
    // if successful, kalloc() will be called and current pid will be saved to page info
    int inc = pages * 4096;
    base = sbrk(inc);
    if (base == (char *)-1)
    {
        printf(1, "[memstress] sbrk failed\n");
        exit();
    }

    // not only allocating physical page, additionally writing actual value to that memory
    // this ensures that actual physical page exists
    if (do_write)
    {
        for (int p = 0; p < pages; p++)
        {
            base[p * 4096] = (char)(p & 0xff);
        }
    }

    sleep(hold_ticks);
    printf(1, "[memstress] pid=%d done\n", pid);
    exit();
}