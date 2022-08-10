/* 
 *
 * Created: 29.03.2018
 *
 * Authors:
 * 
 * Assembled from the code released on Stackoverflow by:
 *   Dennis (instructable.com/member/nqtronix)    |   https://stackoverflow.com/questions/23032002/c-c-how-to-get-integer-unix-timestamp-of-build-time-not-string
 * and
 *   Alexis Wilke                                 |   https://stackoverflow.com/questions/10538444/do-you-know-of-a-c-macro-to-compute-unix-time-and-date
 *
 * Assembled by Jean Rabault
 * 
 * BUILD_UNIX_TIMESTAMP gives the UNIX timestamp (unsigned long integer of seconds since 1st Jan 1970) of compilation from macros using the compiler defined __TIME__ macro.
 * This should include Gregorian calendar leap days, in particular the 29ths of February, 100 and 400 years modulo leaps.
 * 
 * Careful: __TIME__ is the local time of the computer, NOT the UTC time in general!
 * 
 */

#ifndef COMPILE_TIME_H_
#define COMPILE_TIME_H_

// Some definitions for calculation
#define SEC_PER_MIN             60UL
#define SEC_PER_HOUR            3600UL
#define SEC_PER_DAY             86400UL
#define SEC_PER_YEAR            (SEC_PER_DAY*365)

// extracts 1..4 characters from a string and interprets it as a decimal value
#define CONV_STR2DEC_1(str, i)  (str[i]>'0'?str[i]-'0':0)
#define CONV_STR2DEC_2(str, i)  (CONV_STR2DEC_1(str, i)*10 + str[i+1]-'0')
#define CONV_STR2DEC_3(str, i)  (CONV_STR2DEC_2(str, i)*10 + str[i+2]-'0')
#define CONV_STR2DEC_4(str, i)  (CONV_STR2DEC_3(str, i)*10 + str[i+3]-'0')

// Custom "glue logic" to convert the month name to a usable number
#define GET_MONTH(str, i)      (str[i]=='J' && str[i+1]=='a' && str[i+2]=='n' ? 1 :     \
                                str[i]=='F' && str[i+1]=='e' && str[i+2]=='b' ? 2 :     \
                                str[i]=='M' && str[i+1]=='a' && str[i+2]=='r' ? 3 :     \
                                str[i]=='A' && str[i+1]=='p' && str[i+2]=='r' ? 4 :     \
                                str[i]=='M' && str[i+1]=='a' && str[i+2]=='y' ? 5 :     \
                                str[i]=='J' && str[i+1]=='u' && str[i+2]=='n' ? 6 :     \
                                str[i]=='J' && str[i+1]=='u' && str[i+2]=='l' ? 7 :     \
                                str[i]=='A' && str[i+1]=='u' && str[i+2]=='g' ? 8 :     \
                                str[i]=='S' && str[i+1]=='e' && str[i+2]=='p' ? 9 :     \
                                str[i]=='O' && str[i+1]=='c' && str[i+2]=='t' ? 10 :    \
                                str[i]=='N' && str[i+1]=='o' && str[i+2]=='v' ? 11 :    \
                                str[i]=='D' && str[i+1]=='e' && str[i+2]=='c' ? 12 : 0)

// extract the information from the time string given by __TIME__ and __DATE__
#define __TIME_SECONDS__        CONV_STR2DEC_2(__TIME__, 6)
#define __TIME_MINUTES__        CONV_STR2DEC_2(__TIME__, 3)
#define __TIME_HOURS__          CONV_STR2DEC_2(__TIME__, 0)
#define __TIME_DAYS__           CONV_STR2DEC_2(__DATE__, 4)
#define __TIME_MONTH__          GET_MONTH(__DATE__, 0)
#define __TIME_YEARS__          CONV_STR2DEC_4(__DATE__, 7)

// Days in February
#define _UNIX_TIMESTAMP_FDAY(year) \
    (((year) % 400) == 0UL ? 29UL : \
        (((year) % 100) == 0UL ? 28UL : \
            (((year) % 4) == 0UL ? 29UL : \
                28UL)))

// Days in the year
#define _UNIX_TIMESTAMP_YDAY(year, month, day) \
    ( \
        /* January */    day \
        /* February */ + (month >=  2 ? 31UL : 0UL) \
        /* March */    + (month >=  3 ? _UNIX_TIMESTAMP_FDAY(year) : 0UL) \
        /* April */    + (month >=  4 ? 31UL : 0UL) \
        /* May */      + (month >=  5 ? 30UL : 0UL) \
        /* June */     + (month >=  6 ? 31UL : 0UL) \
        /* July */     + (month >=  7 ? 30UL : 0UL) \
        /* August */   + (month >=  8 ? 31UL : 0UL) \
        /* September */+ (month >=  9 ? 31UL : 0UL) \
        /* October */  + (month >= 10 ? 30UL : 0UL) \
        /* November */ + (month >= 11 ? 31UL : 0UL) \
        /* December */ + (month >= 12 ? 30UL : 0UL) \
    )

// get the UNIX timestamp from a digits representation
#define _UNIX_TIMESTAMP(year, month, day, hour, minute, second) \
    ( /* time */ second \
                + minute * SEC_PER_MIN \
                + hour * SEC_PER_HOUR \
    + /* year day (month + day) */ (_UNIX_TIMESTAMP_YDAY(year, month, day) - 1) * SEC_PER_DAY \
    + /* year */ (year - 1970UL) * SEC_PER_YEAR \
                + ((year - 1969UL) / 4UL) * SEC_PER_DAY \
                - ((year - 1901UL) / 100UL) * SEC_PER_DAY \
                + ((year - 1601UL) / 400UL) * SEC_PER_DAY \
    )

// the UNIX timestamp
#define BUILD_UNIX_TIMESTAMP (_UNIX_TIMESTAMP(__TIME_YEARS__, __TIME_MONTH__, __TIME_DAYS__, __TIME_HOURS__, __TIME_MINUTES__, __TIME_SECONDS__))

#endif