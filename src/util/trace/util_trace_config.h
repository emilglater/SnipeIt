#ifndef UTIL_TRACE_CONFIG_H
#define UTIL_TRACE_CONFIG_H

/*
    Compile-time switch for the measurement instrumentation.
    The tests_hw Makefile builds with -DUTIL_TRACE_ENABLED=1.
*/
#ifndef UTIL_TRACE_ENABLED
    #define UTIL_TRACE_ENABLED 0
#endif

typedef enum eUtilTraceConfig
{
    /* Latency samples kept in memory before collection stops. */
    eUTIL_TRACE_MAX_SAMPLES = 4096,
    /* Active Objects that can be tracked at once. */
    eUTIL_TRACE_MAX_AO = 16,
    /* Posts that may be live for an Active Object at any moment. */
    eUTIL_TRACE_PENDING_SLOTS = 64
} eUtilTraceConfig;

#endif
