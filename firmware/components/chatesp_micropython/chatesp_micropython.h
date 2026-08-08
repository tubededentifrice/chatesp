#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CHATESP_MICROPYTHON_MAX_PLOT_POINTS = 128,
    CHATESP_MICROPYTHON_MAX_PLOT_TITLE_BYTES = 48,
};

typedef struct {
    double x[CHATESP_MICROPYTHON_MAX_PLOT_POINTS];
    double y[CHATESP_MICROPYTHON_MAX_PLOT_POINTS];
    size_t count;
    char title[CHATESP_MICROPYTHON_MAX_PLOT_TITLE_BYTES + 1];
    bool ready;
} chatesp_micropython_plot_t;

typedef bool (*chatesp_micropython_cancelled_fn)(void *context);
typedef int64_t (*chatesp_micropython_clock_us_fn)(void *context);

typedef struct {
    char *output;
    size_t output_capacity;
    size_t output_size;
    chatesp_micropython_plot_t *plot;
    chatesp_micropython_cancelled_fn cancelled;
    chatesp_micropython_clock_us_fn clock_us;
    void *callback_context;
    int64_t maximum_duration_us;
    uint32_t maximum_vm_hooks;
} chatesp_micropython_config_t;

typedef enum {
    CHATESP_MICROPYTHON_OK = 0,
    CHATESP_MICROPYTHON_INVALID_ARGUMENT,
    CHATESP_MICROPYTHON_BUSY,
    CHATESP_MICROPYTHON_SCRIPT_ERROR,
    CHATESP_MICROPYTHON_MEMORY_LIMIT,
    CHATESP_MICROPYTHON_OUTPUT_LIMIT,
    CHATESP_MICROPYTHON_TIME_LIMIT,
    CHATESP_MICROPYTHON_CANCELLED,
} chatesp_micropython_result_t;

chatesp_micropython_result_t chatesp_micropython_execute(
    const char *source,
    size_t source_size,
    void *heap,
    size_t heap_size,
    size_t stack_limit_bytes,
    chatesp_micropython_config_t *config);

bool chatesp_micropython_store_plot(
    const double *x,
    const double *y,
    size_t count,
    const char *title,
    size_t title_size);

#ifdef __cplusplus
}
#endif
