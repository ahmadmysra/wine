name mciseq
file mciseq.drv
type win32

import winmm.dll
import user32.dll
import kernel32.dll

@ stdcall DriverProc(long long long long long) MCIMIDI_DriverProc
