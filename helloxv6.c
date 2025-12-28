#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[])
{
    // call hello_number syscall
    // it calls function defined in usys.S and causes T_SYSCALL interrupt
    int res = hello_number(5);
    printf(1, "hello_number(5) returned %d\n", res);
    res = hello_number(-7);
    printf(1, "hello_number(-7) returned %d\n", res);
    exit();
}