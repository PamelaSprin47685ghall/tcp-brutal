/* test_rcvq.c - host tests for the carrier receive-queue repair module.
 *
 * The wire rebuild is compared byte-for-byte with the sender's packing path;
 * the carrier pick and plan are table-driven.  The kernel apply half is not
 * linked here: it needs lock_sock(), tcp_sock and a live socket.
 */
#include <stdio.h>

int test_rcvq_wire(void);
int test_rcvq_plan(void);

int main(void)
{
    int fail = 0;

    fail |= test_rcvq_wire();
    fail |= test_rcvq_plan();
    if (fail)
    {
        printf("RCVQ_TEST_FAIL\n");
        return 1;
    }
    printf("RCVQ_TEST_PASS\n");
    return 0;
}
