#ifndef _PME_LOG_H
#define _PME_LOG_H

#include <stdarg.h>
#include <string.h>
#include <errno.h>

enum log_level {
    LOG_SILENT = 0,
    LOG_ERROR = 1,
    LOG_INFO = 2,
    LOG_DEBUG = 3,
    LOG_LEVEL_LAST,
};

void pme_log_init(enum log_level verbosity);

#ifdef __GNUC__
#define _ATTRIB_PRINTF(start, end) __attribute__((format(printf, start, end)))
#else
#define _ATTRIB_PRINTF(start, end)
#endif

void _pme_log(enum log_level verbosity, const char *format, ...)
    _ATTRIB_PRINTF(2, 3);

#define pme_log(verb, fmt, ...) \
    _pme_log(verb, "[Line %d] " fmt, __LINE__, ##__VA_ARGS__)

#define pme_log_errno(verb, fmt, ...) \
    pme_log(verb, fmt ": %s", ##__VA_ARGS__, strerror(errno))

#endif
