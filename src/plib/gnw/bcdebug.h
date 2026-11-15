// TEMP-HANG-DEBUG: breadcrumbs + watchdog to find where the game blocks.
#pragma once
namespace fallout {
extern volatile const char* g_bc_main;
extern volatile const char* g_bc_audio;
extern volatile unsigned g_bc_loops;
void bc_start_watchdog();
}
#define BC_STR2(x) #x
#define BC_STR(x) BC_STR2(x)
#define BC_MAIN(s) (::fallout::g_bc_main = (s))
#define BC_AUDIO(s) (::fallout::g_bc_audio = (s))
