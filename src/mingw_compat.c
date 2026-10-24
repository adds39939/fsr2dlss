/* Weak fallback for a symbol some MSYS2 mingw-w64 runtime builds (October 2026, GCC 16 / UCRT64)
 * reference from libmingwex's fesetenv.o but fail to define for DLL links:
 *   undefined reference to `__mingw_fe_dfl_env'
 * Upstream defines it next to _fpreset (CRT_fp10.c) for x86; fesetenv() recognises FE_DFL_ENV by
 * address and reinstalls the start-up environment, so the contents only matter when copied. The
 * definition is weak: if the runtime provides the real one, that one wins and this is dropped. */
/* Declared as a raw 8-byte object (the new runtime's fenv_t is two unsigned ints) so this compiles
 * against any mingw-w64 header version. */
__attribute__((weak)) const unsigned int __mingw_fe_dfl_env[2] = { 0x3f00003f, 0 };
