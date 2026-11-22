// The calls the monitor library makes that base API 3.1 does not have (U18 section 5, Q1). Each one is declared here, called where it is needed and defined
// nowhere, so the link of the library lists exactly these as undefined. They are appended in base 3.3 under MultiCore's names (T239.3 section 5): the switch
// to the table member is one line each.
#ifndef TRX64U_PENDING_API_H
#define TRX64U_PENDING_API_H

extern "C" {

// sys.yield: lets the tasks of the same priority run. plat_yield (lib_monitor.cc) for trxmon's waits on the debug block (mach.c wait_status); SvcSys.sleep_ms(0)
// sleeps one tick, it is no yield. BOUNDARY.md: FreeRTOS (MultiCore's monitor.cc called taskYIELD, a macro, so the raw list does not show it).
void sys_yield(void);

// sync.enter / sync.leave: a short critical section, nothing that blocks inside. lib_monitor.cc's engine guard (engine_try): the poll task and the menu task
// both reach trxmon's engine, and the flag that gives it to one of them is set inside. BOUNDARY.md: FreeRTOS (critical sections, mutexes: "no mutex in the API").
void sync_enter(void);
void sync_leave(void);

}

#endif
