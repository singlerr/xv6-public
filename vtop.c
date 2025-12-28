#include "types.h"
#include "user.h"

#define PGSIZE 4096

int main(int argc, char *argv[])
{
    int pid = getpid();
    int r;
    uint32_t va;
    uint32_t pa;
    uint32_t flags;
    uint32_t hits;
    uint32_t misses;

    if (argc < 2)
    {
        printf(1, "usage: %s [va]\n", argv[0]);
        exit();
    }

    va = (uint32_t)atoi(argv[1]);

    // get hits/misses count
    if (tlbinfo(&hits, &misses) < 0)
    {
        printf(1, "tlbinfo error!\n");
        exit();
    }
    // from the given va, incrementing PGSIZE and get pa and flags
    for (; (r = vtop((void *)va, &pa, &flags)) > 0; va += PGSIZE)
    {
        printf(1, "VA= %p -> PA= %p, flags= %d, hit= %d, miss= %d\n", va, pa, flags, hits, misses);
    }

    exit();
}