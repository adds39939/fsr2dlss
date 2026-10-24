/* Definition for a symbol current MSYS2 mingw-w64 runtimes (October 2026, GCC 16 / UCRT64) leave
 * undefined when linking a DLL:
 *   libmingwex.a(fesetenv.o): undefined reference to `__mingw_fe_dfl_env'
 * Upstream mingw-w64 defines it for x86/x64 next to _fpreset in the executable start-up objects
 * (CRT_fp10.c / CRT_fp8.c), which are never linked into a DLL, while libmingwex's fesetenv.o still
 * references it. fesetenv() recognises FE_DFL_ENV by address and reinstalls the start-up environment,
 * so the contents only matter if something copies it; these are the upstream default values (the
 * runtime's fenv_t is two unsigned ints; declared raw so this compiles with any header version).
 * A weak definition does not work here: PE weak externals do not satisfy .refptr references.
 * If a future runtime defines it for DLLs as well, the link will report a duplicate and this file
 * can simply be removed from the build. */
const unsigned int __mingw_fe_dfl_env[2] = { 0x3f00003f, 0 };
