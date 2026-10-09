#pragma once
#include "tcg.h"

/*
 * Hand control to the image entry point without leaving a return address that
 * points into this PIC.
 *
 * A plain call pushes the address of the instruction after it, which lives in
 * fileless memory. Any stack walk then runs off the end of the module list. This
 * instead builds a synthetic stack whose bottom looks like a thread that started
 * through the normal Windows path:
 *
 *     ...  RtlUserThreadStart -> BaseThreadInitThunk -> entry point
 *
 * and enters the entry point with NtContinue, which does not push anything.
 *
 * The distances between the synthetic frames are computed from the unwind info
 * of the two functions rather than hardcoded, so they follow the OS build.
 *
 * Does not return.
 */
void rdll_transfer_execution(void * entry, void * image, DWORD reason);