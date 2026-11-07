#ifndef PORT_CPU_CLOCK_H
#define PORT_CPU_CLOCK_H

void cpu_clock_start(void);
void cpu_clock_keep(void); // call about once a second
void cpu_clock_stop(void);
unsigned cpu_clock_mhz(void); // 0 when not raised

#endif
