// Third canary: the port's relocator and full libnx/newlib startup, with no port code.
// Reaching this main means everything before the game-specific code works.
#include <switch.h>

int main(void)
{
    svcSleepThread(5000000000LL);
    return 0;
}
