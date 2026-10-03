; Windows 7 support (see win7.cpp): our own definitions of three Windows 8 imports, so the linker
; takes these instead of kernel32.lib's and the DLL doesn't import them.
.686
.model flat

EXTERN _ps_GetSystemTimePreciseAsFileTime@4:PROC
EXTERN _ps_CreateFile2@20:PROC
EXTERN _ps_CopyFile2@12:PROC

.data
PUBLIC __imp__GetSystemTimePreciseAsFileTime@4
__imp__GetSystemTimePreciseAsFileTime@4 DD _ps_GetSystemTimePreciseAsFileTime@4
PUBLIC __imp__CreateFile2@20
__imp__CreateFile2@20 DD _ps_CreateFile2@20
PUBLIC __imp__CopyFile2@12
__imp__CopyFile2@12 DD _ps_CopyFile2@12

END
