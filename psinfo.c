#include "types.h"
#include "stat.h"
#include "user.h"

// convert process state enum to string
// since cannot access to proc.h, we need to distinguish with int
static char *s2str(int s)
{
    switch (s)
    {
    case 0:
        return "UNUSED";
    case 1:
        return "EMBRYO";
    case 2:
        return "SLEEPING";
    case 3:
        return "RUNNABLE";
    case 4:
        return "RUNNING";
    case 5:
        return "ZOMBIE";
    }
    return "UNKNOWN";
}

int main(int argc, char *argv[])
{
    // struct procinfo has same structure with struct k_procinfo in sysproc.c
    // this allows it to access valid value though using different struct identifiers
    struct procinfo info;
    // parse str to int
    int pid = (argc >= 2) ? atoi(argv[1]) : 0;

    // call get_procinfo syscall
    if (get_procinfo(pid, &info) < 0)
    {
        printf(1, "psinfo: failed (pid=%d)\n", pid);
        exit();
    }
    // print process state
    printf(1, "PID=%d PPID=%d STATE=%s SZ=%d NAME=%s\n", info.pid, info.ppid, s2str(info.state), info.sz, info.name);
    exit();
}