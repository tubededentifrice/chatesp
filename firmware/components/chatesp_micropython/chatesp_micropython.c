#include "chatesp_micropython.h"

#include <stdlib.h>
#include <string.h>

#include "py/compile.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/objexcept.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "shared/runtime/gchelper.h"

typedef struct {
    chatesp_micropython_config_t *config;
    int64_t started_us;
    uint32_t vm_hooks;
    bool enforcing_limits;
    bool cancelled;
    bool timed_out;
    bool output_limited;
} execution_context_t;

static execution_context_t *active_context;

static void clear_plot(chatesp_micropython_plot_t *plot) {
    if (plot != NULL) {
        memset(plot, 0, sizeof(*plot));
    }
}

void chatesp_micropython_vm_hook(void) {
    execution_context_t *context = active_context;
    if (context == NULL || !context->enforcing_limits) {
        return;
    }
    ++context->vm_hooks;
    if (context->config->cancelled != NULL &&
        context->config->cancelled(context->config->callback_context)) {
        context->cancelled = true;
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("cancelled"));
    }
    const bool operation_limit =
        context->config->maximum_vm_hooks != 0 &&
        context->vm_hooks >= context->config->maximum_vm_hooks;
    bool time_limit = false;
    if (context->config->clock_us != NULL &&
        context->config->maximum_duration_us > 0) {
        const int64_t now_us =
            context->config->clock_us(context->config->callback_context);
        time_limit = now_us - context->started_us >=
            context->config->maximum_duration_us;
    }
    if (operation_limit || time_limit) {
        context->timed_out = true;
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("time limit"));
    }
}

void mp_hal_stdout_tx_strn_cooked(const char *text, size_t size) {
    execution_context_t *context = active_context;
    if (context == NULL || context->config == NULL || text == NULL ||
        size == 0) {
        return;
    }
    chatesp_micropython_config_t *config = context->config;
    const size_t maximum_output = config->output_capacity - 1;
    const size_t remaining = maximum_output > config->output_size
        ? maximum_output - config->output_size
        : 0;
    if (size > remaining) {
        context->output_limited = true;
        size = remaining;
    }
    if (size != 0) {
        memcpy(config->output + config->output_size, text, size);
        config->output_size += size;
    }
    if (config->output_capacity != 0) {
        const size_t terminator = config->output_size < maximum_output
            ? config->output_size
            : maximum_output;
        config->output[terminator] = '\0';
    }
    if (context->output_limited && context->enforcing_limits) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("output limit"));
    }
}

bool chatesp_micropython_store_plot(
    const double *x,
    const double *y,
    size_t count,
    const char *title,
    size_t title_size) {
    execution_context_t *context = active_context;
    if (context == NULL || context->config == NULL ||
        context->config->plot == NULL || x == NULL || y == NULL ||
        count < 2 || count > CHATESP_MICROPYTHON_MAX_PLOT_POINTS ||
        (title_size != 0 && title == NULL) ||
        title_size > CHATESP_MICROPYTHON_MAX_PLOT_TITLE_BYTES) {
        return false;
    }
    chatesp_micropython_plot_t *plot = context->config->plot;
    clear_plot(plot);
    memcpy(plot->x, x, count * sizeof(double));
    memcpy(plot->y, y, count * sizeof(double));
    if (title_size != 0) {
        memcpy(plot->title, title, title_size);
    }
    plot->title[title_size] = '\0';
    plot->count = count;
    plot->ready = true;
    return true;
}

static chatesp_micropython_result_t classify_exception(
    execution_context_t *context, void *exception) {
    if (context->cancelled) {
        return CHATESP_MICROPYTHON_CANCELLED;
    }
    if (context->timed_out) {
        return CHATESP_MICROPYTHON_TIME_LIMIT;
    }
    if (context->output_limited) {
        return CHATESP_MICROPYTHON_OUTPUT_LIMIT;
    }
    if (mp_obj_exception_match(
            MP_OBJ_FROM_PTR(exception), MP_OBJ_FROM_PTR(&mp_type_MemoryError))) {
        return CHATESP_MICROPYTHON_MEMORY_LIMIT;
    }
    return CHATESP_MICROPYTHON_SCRIPT_ERROR;
}

chatesp_micropython_result_t chatesp_micropython_execute(
    const char *source,
    size_t source_size,
    void *heap,
    size_t heap_size,
    size_t stack_limit_bytes,
    chatesp_micropython_config_t *config) {
    if (source == NULL || source_size == 0 || heap == NULL || heap_size < 4096 ||
        stack_limit_bytes < 2048 || config == NULL || config->output == NULL ||
        config->output_capacity < 2 || config->plot == NULL ||
        config->clock_us == NULL || config->maximum_duration_us <= 0 ||
        active_context != NULL) {
        return active_context != NULL ? CHATESP_MICROPYTHON_BUSY
                                      : CHATESP_MICROPYTHON_INVALID_ARGUMENT;
    }

    config->output_size = 0;
    config->output[0] = '\0';
    clear_plot(config->plot);
    execution_context_t context = {
        .config = config,
        .started_us = config->clock_us(config->callback_context),
        .vm_hooks = 0,
        .enforcing_limits = false,
        .cancelled = false,
        .timed_out = false,
        .output_limited = false,
    };
    active_context = &context;

    char stack_top;
    mp_stack_set_top(&stack_top);
    mp_stack_set_limit(stack_limit_bytes);
    gc_init(heap, (uint8_t *)heap + heap_size);

    chatesp_micropython_result_t result = CHATESP_MICROPYTHON_OK;
    volatile bool initialized = false;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_init();
        initialized = true;
        context.enforcing_limits = true;
        mp_lexer_t *lexer = mp_lexer_new_from_str_len(
            MP_QSTR__lt_stdin_gt_, source, source_size, 0);
        const qstr source_name = lexer->source_name;
        mp_parse_tree_t parse_tree = mp_parse(lexer, MP_PARSE_FILE_INPUT);
        mp_obj_t module_function =
            mp_compile(&parse_tree, source_name, true);
        mp_call_function_0(module_function);
        nlr_pop();
    } else {
        context.enforcing_limits = false;
        result = classify_exception(&context, nlr.ret_val);
        if (initialized) {
            mp_obj_print_exception(
                &mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
        }
    }

    context.enforcing_limits = false;
    if (result == CHATESP_MICROPYTHON_OK && context.output_limited) {
        result = CHATESP_MICROPYTHON_OUTPUT_LIMIT;
    }
    if (initialized) {
        mp_deinit();
    }
    active_context = NULL;
    return result;
}

#if MICROPY_ENABLE_GC
void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}
#endif

void nlr_jump_fail(void *value) {
    (void)value;
    abort();
}
