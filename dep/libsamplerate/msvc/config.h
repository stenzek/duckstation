#include <stdbool.h>

/* Name of package */
#define PACKAGE "libsamplerate"

/* Version number of package */
#define VERSION "0.1.9"

/* Target processor clips on negative float to int conversion. */
#define CPU_CLIPS_NEGATIVE 0

/* Target processor clips on positive float to int conversion. */
#define CPU_CLIPS_POSITIVE 0

/* Target processor is big endian. */
#define CPU_IS_BIG_ENDIAN 0

/* Target processor is little endian. */
#define CPU_IS_LITTLE_ENDIAN 1

/* Define to 1 if you have the `alarm' function. */
/* #undef HAVE_ALARM */

/* Define to 1 if you have the <alsa/asoundlib.h> header file. */
/* #undef HAVE_ALSA */

/* Set to 1 if you have libfftw3. */
/* #undef HAVE_FFTW3 */

/* Define if you have C99's lrint function. */
/* #undef HAVE_LRINT */

/* Define if you have C99's lrintf function. */
/* #undef HAVE_LRINTF */

/* Define if you have signal SIGALRM. */
/* #undef HAVE_SIGALRM */

/* Define to 1 if you have the `signal' function. */
/* #undef HAVE_SIGNAL */

/* Set to 1 if you have libsndfile. */
/* #undef HAVE_SNDFILE */

/* Define to 1 if you have the <stdbool.h> header file. */
/* #undef HAVE_STDBOOL_H */

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <sys/times.h> header file. */
/* #undef HAVE_SYS_TIMES_H */

/* Define to 1 if you have the <unistd.h> header file. */
/* #undef HAVE_UNISTD_H */

/* define fast samplerate convertor */
#define ENABLE_SINC_FAST_CONVERTER 1

/* define balanced samplerate convertor */
#define ENABLE_SINC_MEDIUM_CONVERTER 1

/* define best samplerate convertor */
/* #define ENABLE_SINC_BEST_CONVERTER 1 */

/* The size of `int', as computed by sizeof. */
#define SIZEOF_INT 4

/* The size of `long', as computed by sizeof. */
#define SIZEOF_LONG 4
