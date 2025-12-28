#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[])
{
    int id;

    // call snapshot_create syscall
    id = snapshot_create();

    // if return value is -1, then it means snapshot creation failed due to various reasons, such as snapshot folder creation failed
    if (id == -1)
    {
        printf(2, "snapshot_create failed\n");
        exit();
    }
    // if return value is -2, then it means snapshot creation failed due to exhaustion of inodes
    else if (id == -2)
    {
        printf(2, "snapshot_create failed: out of inodes\n");
        exit();
    }

    printf(1, "snapshot created with id: %d\n", id);
    exit();
}