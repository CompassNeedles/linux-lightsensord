Author: Sami Mourad

Please check out Repository Website for instructions.

Implements an Android daemon process to write light sensor data to the kernel.
This is contained in light_d/

Implements a Linux kernel API (system calls) to detect light events based on
sampling frequency and light intensity requirements as specified by the user.
This is contained in flo-kernel/kernel/light.c with interface defined in
flo-kernel/include/linux/light.h

A "holistic" test is provided in test/
It tests all system calls at once which is obviously not the cleanest way.
Testing done on the emulator using fake light sensor data rather than
actual device.

To do: return the appropriate messages on error from the kernel - test on device
