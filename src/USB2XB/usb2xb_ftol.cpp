/*---------------------------------------------------------------------------
    usb2xb_ftol.cpp -- supply __ftol2_sse for RXDK.

    minimp3's scalar synthesis contains float->integer conversions.  With the
    RXDK/MSVC toolchain those conversions can emit a reference to the CRT
    helper __ftol2_sse, which RXDK does not provide.

    IMPORTANT:
      Build THIS FILE ONLY with Whole Program Optimization disabled (/GL-).
      The rest of USB2XB may keep its normal Release optimization settings.

    This follows DarkDash's proven RXDK implementation:
      - truncate toward zero, matching a C cast
      - preserve/restore the caller's x87 control word
      - return the full 64-bit conversion result in edx:eax

    Source identifier _ftol2_sse decorates to linker symbol __ftol2_sse on
    32-bit x86 C linkage.
---------------------------------------------------------------------------*/

#pragma warning(disable : 4731)

extern "C" __declspec(naked) void __cdecl _ftol2_sse(void)
{
    __asm
    {
        push    ebp
        mov     ebp, esp
        sub     esp, 16

        /* save caller x87 control word */
        fnstcw  word ptr[ebp - 2]

        /* install round-toward-zero (chop) */
        movzx   eax, word ptr[ebp - 2]
        or ah, 0Ch
        mov     word ptr[ebp - 4], ax
        fldcw   word ptr[ebp - 4]

        /* convert st(0), pop it, return qword in edx:eax */
        fistp   qword ptr[ebp - 12]

        /* restore caller control word */
        fldcw   word ptr[ebp - 2]

        mov     eax, dword ptr[ebp - 12]
        mov     edx, dword ptr[ebp - 8]

        mov     esp, ebp
        pop     ebp
        ret
    }
}

#pragma warning(default : 4731)