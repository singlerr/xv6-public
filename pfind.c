#include "types.h"
#include "user.h"

int main(int argc, char *argv[])
{
    uint32_t pa;
    char c;
    int max = 20;
    int r;
    struct vlist *list, *p;

    if (argc < 2)
    {
        printf(1, "usage : %s [pa]\n", argv[0]);
        exit();
    }

    while ((c = getopts(argc, argv, "m:")) != -1)
    {
        switch (c)
        {
        case 'm':
            max = atoi(argv[optind]);
            break;

        default:
            break;
        }
    }

    if (max <= 0)
    {
        printf(1, "max must bigger than 0\n");
        exit();
    }

    pa = atoi(argv[1]);
    list = (struct vlist *)malloc(sizeof(struct vlist) * max);

    if ((r = phys2virt(pa, list, max)) < 0)
    {
        printf(1, "phys2virt error!\n");
        exit();
    }

    printf(1, "%p -> ", pa);

    for (p = list; p < list + r; p++)
    {
        printf(1, "pid=%d,va_page=%d,flags=%d ", p->pid, p->va, p->flags);
    }

    printf(1, "\n");

    exit();
}