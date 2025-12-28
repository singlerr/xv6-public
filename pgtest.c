#include "types.h"
#include "user.h"

#define PGSIZE 4096
#define PTE_P 0x001 // Present
#define PTE_W 0x002 // Writeable
#define PTE_U 0x004 // User
void test_sw_vtop(void);
void test_ipt_lookup(void);
void test_cow_scenario(void);
void test_consistency(void);

int main(int argc, char *argv[])
{
    printf(1, "=== Test ===\n\n");

    test_sw_vtop();
    test_ipt_lookup();
    test_cow_scenario();
    test_consistency();
    printf(1, "\n=== Test End ===\n");
    exit();
}

// test sw_vtop syscall
// 1. allocate one page and print physical address and PTE flags
// 2. print physical address and PTE flags of virtual address of code segment(address of this function)
// 3. tries to get physical address and PTE flags of invalid address
void test_sw_vtop(void)
{
    printf(1, "[TEST 1] sw_vtop\n");
    uint32_t pa, flags;
    char *test_addr;
    test_addr = sbrk(PGSIZE);
    *test_addr = 'A';

    if (vtop(test_addr, &pa, &flags) > 0)
    {
        printf(1, "Heap VA: 0x%x -> PA: 0x%x, Flags: 0x%x\n",
               test_addr, pa, flags);
    }

    if (vtop(&test_sw_vtop, &pa, &flags) > 0)
    {
        printf(1, "Code VA: 0x%x -> PA: 0x%x, Flags: 0x%x\n",
               &test_sw_vtop, pa, flags);
        if (!(flags & PTE_W))
        {
            printf(1, "Code segment is read only\n");
        }
    }

    if (vtop((void *)0xDEADDEAD, &pa, &flags) <= 0)
    {
        printf(1, "vtop returned 0 or -1 for invalid address\n");
    }

    printf(1, "\n");
}

// test ipt table
// 1. allocate two page
// 2. get physical address of each page using vtop
// 3. print chained ipt entries pointing to each physical address using phys2virt
void test_ipt_lookup(void)
{
    printf(1, "[TEST 2] IPT\n");

    char *addr1 = sbrk(PGSIZE);
    char *addr2 = sbrk(PGSIZE);
    *addr1 = 'X';
    *addr2 = 'Y';

    uint32_t pa1, pa2, flags;
    vtop(addr1, &pa1, &flags);
    vtop(addr2, &pa2, &flags);
    struct vlist results[10];

    int count = phys2virt(pa1 & ~0xFFF, results, 10);
    printf(1, "PA: 0x%x => \n", pa1 & ~0xFFF);
    for (int i = 0; i < count; i++)
    {
        printf(1, "PID=%d, VA=0x%x, Flags=0x%x\n",
               results[i].pid, results[i].va, results[i].flags);
    }

    printf(1, "\n");
}

// test ipt entry chained when more than two virtual addresses pointing to same physical page
// 1. allocate and print parent's virtual page and physical address
// 2. for 3 times, fork child process and ensure that child's virtual address also pointing to same physical address
// 2. and check copy-on-write to simulate memory modification(number of ipt chain will be decremented)
void test_cow_scenario(void)
{
    printf(1, "[TEST 3] Check ipt table when copy on write\n");

    char *shared = sbrk(PGSIZE);
    memset(shared, 'S', 100);

    uint32_t pa_parent, flags;
    vtop(shared, &pa_parent, &flags);
    printf(1, "Parent: VA=0x%x -> PA=0x%x\n", shared, pa_parent);

    for (int i = 0; i < 3; i++)
    {
        int pid = fork();
        if (pid == 0)
        {
            printf(1, "[Fork %d] \n", i);
            uint32_t pa_child;
            vtop(shared, &pa_child, &flags);
            printf(1, "Before child write new value - VA=0x%x -> PA=0x%x\n",
                   shared, pa_child);

            struct vlist results[100];
            int count = phys2virt(pa_child, results, 100);
            printf(1, "PA 0x%x has %d chains - ", pa_child, count);
            for (int j = 0; j < count; j++)
            {
                printf(1, "(PID=%d, VA=0x%x, Flags=%d) ", results[j].pid, results[j].va, results[j].flags);
            }

            printf(1, "\n");

            // trigger cow
            shared[0] = 'C';
            vtop(shared, &pa_child, &flags);
            printf(1, "After child write new value(trigger cow): VA=0x%x -> PA=0x%x\n",
                   shared, pa_child);
            count = phys2virt(pa_child, results, 100);
            printf(1, "After child write PA 0x%x has %d chains - ", pa_child, count);
            for (int j = 0; j < count; j++)
            {
                printf(1, "(PID=%d, VA=0x%x, Flags=%d) ", results[j].pid, results[j].va, results[j].flags);
            }
            printf(1, "\n");
            exit();
        }
        else
        {
            wait();
        }
    }

    struct vlist results[100];
    int count = phys2virt(pa_parent, results, 100);
    printf(1, "After child process all killed\n");
    printf(1, "PA 0x%x has %d chains - ", pa_parent, count);
    for (int j = 0; j < count; j++)
    {
        printf(1, "(PID=%d, VA=0x%x, Flags=%d) ", results[j].pid, results[j].va, results[j].flags);
    }

    printf(1, "\n");
}

// test consistency of ipt table and tlb
// 1. allocate three pages and write something to them
// 2. check deallocated pages cannot be accessed via vtop because ipt entry destroyed after deallocation
void test_consistency(void)
{
    printf(1, "[TEST 4] Consistency Check\n");

    char *addr = sbrk(PGSIZE * 3);
    for (int i = 0; i < 3; i++)
    {
        addr[i * 4096] = 'A' + i;
    }

    // dealloc page
    sbrk(-PGSIZE);

    for (int i = 0; i < 3; i++)
    {
        uint32_t pa, flags;
        int ret = vtop(addr + i * 4096, &pa, &flags);

        if (i == 2 && ret < 0)
        {
            printf(1, "Deallocated page not accessible\n");
        }
        else if (i < 2 && ret == 2)
        {
            printf(1, "Page %d: VA=0x%x -> PA=0x%x (valid)\n",
                   i, addr + i * 4096, pa);
        }
    }

    printf(1, "\n");
}