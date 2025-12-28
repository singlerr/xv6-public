#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[])
{
    int id;

    if (argc != 2)
    {
        printf(2, "Usage: %s snapshot_id\n", argv[0]);
        exit();
    }

    id = atoi(argv[1]);

    // call snapshot_delete syscall to delete all captured snapshot files with id
    if (snapshot_delete(id) < 0)
    {
        printf(2, "snapshot_delete failed for id: %d\n", id);
        exit();
    }

    printf(1, "deleted snapshot id: %d\n", id);
    exit();
}