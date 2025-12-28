#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"

// receives two parameters to program
// first file name and second new contents
int main(int argc, char *argv[])
{
    // direct addresses
    uint addrs[NDIRECT + 1];
    // indirect addresses
    uint indirect[NINDIRECT];
    int i, j;
    int fd;

    if (argc != 2)
    {
        printf(2, "Usage: %s file\n", argv[0]);
        exit();
    }

    // get direct addrs
    if (get_addrs(argv[1], addrs) < 0)
    {
        printf(2, "cannot get addresses for %s\n", argv[1]);
        exit();
    }

    // print all direct block addresses
    for (i = 0; i < NDIRECT; i++)
    {
        // only print allocated addresses
        if (addrs[i] != 0)
        {
            printf(1, "addr[%d]: %x\n", i, addrs[i]);
        }
    }

    // if that inode allocated indirect addresses, search for indirect addresses
    if (addrs[NDIRECT] != 0)
    {
        printf(1, "addr[%d]: %x(INDIRECT POINTER)\n", NDIRECT, addrs[NDIRECT]);

        // get indirect addrs
        if (get_indirect_addrs(argv[1], indirect) >= 0)
        {
            for (j = 0; j < NINDIRECT; j++)
            {
                // only print allocated block addresses
                if (indirect[j] != 0)
                {
                    printf(1, "addr[%d]->[%d](bn: %d): %x\n",
                           NDIRECT, j, NDIRECT + j, indirect[j]);
                }
            }
        }
    }

    exit();
}