// The game runs at the stock 1020 MHz CPU clock by default.  Creating sd:/switch/cod_st/boost
// raises it to 1785 MHz, the clock retail games get on loading screens, for testing only: the
// port is meant to be playable at stock clocks.
#include "port/cpu_clock.h"

#include <stdio.h>
#include <switch.h>
#include <unistd.h>

#define OPT_IN "sdmc:/switch/cod_st/boost"
#define BOOST_HZ 1785000000u
#define SESSION_ATTRIBUTE 3

static ClkrstSession g_session;
static u32 g_normal_hz;
static int g_active;
static int g_open; // session open, for reading the clock even when not raising it

void cpu_clock_start(void)
{
    Result rc = clkrstInitialize();
    if (R_SUCCEEDED(rc))
        rc = clkrstOpenSession(&g_session, PcvModuleId_CpuBus, SESSION_ATTRIBUTE);
    if (R_SUCCEEDED(rc)) {
        g_open = 1;
        rc = clkrstGetClockRate(&g_session, &g_normal_hz);
    }
    if (R_SUCCEEDED(rc) && access(OPT_IN, F_OK) != 0) {
        // watch.log still shows the real clock, so an overclocking tool's change is visible.
        fprintf(stderr, "cpu: stock clock, %u MHz (create %s to raise it)\n", g_normal_hz / 1000000u, OPT_IN);
        return;
    }
    if (R_SUCCEEDED(rc))
        rc = clkrstSetClockRate(&g_session, BOOST_HZ);
    if (R_FAILED(rc)) {
        fprintf(stderr, "cpu: cannot set the clock, rc=0x%x\n", rc);
        return;
    }
    g_active = 1;
    fprintf(stderr, "cpu: %u MHz, was %u MHz\n", BOOST_HZ / 1000000u, g_normal_hz / 1000000u);
}

// The system puts its own clock back when, for example, the console is docked.
void cpu_clock_keep(void)
{
    u32 hz = 0;
    if (g_active && R_SUCCEEDED(clkrstGetClockRate(&g_session, &hz)) && hz != BOOST_HZ)
        clkrstSetClockRate(&g_session, BOOST_HZ);
}

void cpu_clock_stop(void)
{
    if (g_active)
        clkrstSetClockRate(&g_session, g_normal_hz);
    g_active = 0;
    if (!g_open)
        return;
    g_open = 0;
    clkrstCloseSession(&g_session);
    clkrstExit();
}

unsigned cpu_clock_mhz(void)
{
    u32 hz = 0;
    return g_open && R_SUCCEEDED(clkrstGetClockRate(&g_session, &hz)) ? hz / 1000000u : 0;
}
