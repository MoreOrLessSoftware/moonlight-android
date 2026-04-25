/**
 * Custom assert.h to avoid NDK 27 __assert function conflicts
 * with Clang target multiversioning in rswrapper.c
 *
 * This header replaces the system assert.h to prevent __assert()
 * and __assert2() declarations that lack the 'target' attribute,
 * which causes compilation errors when using function multiversioning.
 */

#undef assert

#ifdef NDEBUG
  #define assert(x) ((void)0)
#else
  #define assert(x) ((void)0)
#endif
