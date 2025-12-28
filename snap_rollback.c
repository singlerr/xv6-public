#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[])
{
    int id;
    int ret;

    if (argc != 2)
    {
        printf(2, "Usage: %s snapshot_id\n", argv[0]);
        exit();
    }

    id = atoi(argv[1]);

    ret = snapshot_rollback(id);
    // if return value is -1, it means that snapshot rollback failed due to various reasons such as failure of file replacement
    if (ret == -1)
    {
        printf(2, "snapshot_rollback failed for id: %d\n", id);
        exit();
    }
    // if return value is -2, it means that snapshot rollback failed due to exhaustion of inodes
    else if (ret == -2)
    {
        printf(2, "snapshot_rollback failed for id: %d, out of inodes\n", id);
        exit();
    }

    printf(1, "snapshot_rollback succeeded with snapshot id: %d\n", id);
    exit();
}