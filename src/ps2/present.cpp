/*
    NooDS-PS2 presentation pacing.

    gsKit_sync_flip() always waits for the NEXT vsync edge (gsKit_vsync_wait()
    clears the CSR vsync flag and then spins until it is set again). Whenever a
    DS frame takes longer than one display period to emulate, that wait is pure
    idle time: ~8.3 ms on average, which is the roughly constant ~9.7 ms
    "presentMs" seen in the profile logs.

    gskit is a static library with one object per function, so this definition
    replaces gsKit's own gsKit_sync_flip() at link time. The behaviour is the
    same while the emulator is faster than the display (it still waits for
    vsync, keeping 60 Hz pacing). When the previous present was at least one
    NTSC field ago it waits for the GS to finish drawing the back buffer instead
    and flips immediately, which can show a tear line for a field.

    Build with -DNOODS_PS2_PRESENT_VSYNC_ALWAYS (make -f Makefile.ps2
    PRESENT=vsync) to restore the original always-wait behaviour.
*/

#include <ctime>
#include <gsKit.h>

namespace
{
#ifndef NOODS_PS2_PRESENT_VSYNC_ALWAYS
    // One NTSC field (59.94 Hz) in clock() ticks
    inline clock_t vsyncPeriodTicks()
    {
        return static_cast<clock_t>(
            static_cast<unsigned long long>(CLOCKS_PER_SEC) * 1001ull / 60000ull);
    }

    clock_t lastPresent = 0;
#endif
}

extern "C" void gsKit_sync_flip(GSGLOBAL *gsGlobal)
{
    if (!gsGlobal->FirstFrame)
    {
#ifndef NOODS_PS2_PRESENT_VSYNC_ALWAYS
        const clock_t now = clock();
        const bool behind = lastPresent != 0 && (now - lastPresent) >= vsyncPeriodTicks();
        if (behind)
        {
            // The vsync we would wait for is already late: just make sure the GS
            // finished the back buffer so a half-drawn frame is never shown.
            gsKit_finish();
        }
        else
#endif
        {
            gsKit_vsync_wait();
        }

        if (gsGlobal->DoubleBuffering == GS_SETTING_ON)
        {
            GS_SET_DISPFB2(gsGlobal->ScreenBuffer[gsGlobal->ActiveBuffer & 1] / 8192,
                gsGlobal->Width / 64, gsGlobal->PSM, 0, 0);

            gsGlobal->ActiveBuffer ^= 1;
        }
    }

    gsKit_setactive(gsGlobal);

#ifndef NOODS_PS2_PRESENT_VSYNC_ALWAYS
    lastPresent = clock();
#endif
}
