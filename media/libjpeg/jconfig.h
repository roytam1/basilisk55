/* jconfig.h.  Generated from jconfig.h.in by configure, then manually edited
   for Mozilla. */

/* Export libjpeg v6.2's ABI. */
#define JPEG_LIB_VERSION 62

/* libjpeg-turbo version */
#define LIBJPEG_TURBO_VERSION 2.1.5.1

/* libjpeg-turbo version in integer form */
#define LIBJPEG_TURBO_VERSION_NUMBER  2001005

/* Support arithmetic encoding */
/*#undef C_ARITH_CODING_SUPPORTED */

/* Support arithmetic decoding */
/*#undef D_ARITH_CODING_SUPPORTED */

/* Support in-memory source/destination managers */
#define MEM_SRCDST_SUPPORTED 1

/* Use accelerated SIMD routines. */
#define WITH_SIMD 1

/*
 * Define BITS_IN_JSAMPLE as either
 *   8   for 8-bit sample values (the usual setting)
 *   12  for 12-bit sample values
 * Only 8 and 12 are legal data precisions for lossy JPEG according to the
 * JPEG standard, and the IJG code does not support anything else!
 * We do not support run-time selection of data precision, sorry.
 */

#define BITS_IN_JSAMPLE  8      /* use 8 or 12 */

/* Define if your (broken) compiler shifts signed values as if they were
   unsigned. */
/* #undef RIGHT_SHIFT_IS_UNSIGNED */

/* Define to 1 if type `char' is unsigned and you are not using gcc.  */
#ifndef __CHAR_UNSIGNED__
/* # undef __CHAR_UNSIGNED__ */
#endif

/* Define to empty if `const' does not conform to ANSI C. */
/* #undef const */

/* Define to `unsigned int' if <sys/types.h> does not define. */
/* #undef size_t */

/* The size of `size_t', as computed by sizeof. */
#ifdef HAVE_64BIT_BUILD
#define SIZEOF_SIZE_T 8
#else
#define SIZEOF_SIZE_T 4
#endif
