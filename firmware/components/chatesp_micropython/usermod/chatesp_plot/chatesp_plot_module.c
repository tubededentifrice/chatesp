#include <math.h>
#include <string.h>

#include "chatesp_micropython.h"
#include "py/obj.h"
#include "py/runtime.h"

static mp_obj_t chatesp_plot_line(size_t argument_count, const mp_obj_t *args) {
    size_t x_count = 0;
    size_t y_count = 0;
    mp_obj_t *x_items = NULL;
    mp_obj_t *y_items = NULL;
    mp_obj_get_array(args[0], &x_count, &x_items);
    mp_obj_get_array(args[1], &y_count, &y_items);
    if (x_count != y_count || x_count < 2 ||
        x_count > CHATESP_MICROPYTHON_MAX_PLOT_POINTS) {
        mp_raise_ValueError(MP_ERROR_TEXT("plot needs 2 to 128 matching points"));
    }

    double x[CHATESP_MICROPYTHON_MAX_PLOT_POINTS];
    double y[CHATESP_MICROPYTHON_MAX_PLOT_POINTS];
    for (size_t index = 0; index < x_count; ++index) {
        x[index] = mp_obj_get_float(x_items[index]);
        y[index] = mp_obj_get_float(y_items[index]);
        if (!isfinite(x[index]) || !isfinite(y[index])) {
            mp_raise_ValueError(MP_ERROR_TEXT("plot values must be finite"));
        }
    }

    const char *title = "";
    size_t title_size = 0;
    if (argument_count == 3) {
        if (!mp_obj_is_str(args[2])) {
            mp_raise_TypeError(MP_ERROR_TEXT("plot title must be text"));
        }
        title = mp_obj_str_get_data(args[2], &title_size);
        if (title_size > CHATESP_MICROPYTHON_MAX_PLOT_TITLE_BYTES) {
            mp_raise_ValueError(MP_ERROR_TEXT("plot title is too long"));
        }
        if (memchr(title, '\0', title_size) != NULL) {
            mp_raise_ValueError(MP_ERROR_TEXT("plot title has a null byte"));
        }
    }
    if (!chatesp_micropython_store_plot(
            x, y, x_count, title, title_size)) {
        mp_raise_msg(
            &mp_type_RuntimeError, MP_ERROR_TEXT("plot is unavailable"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    chatesp_plot_line_object, 2, 3, chatesp_plot_line);

static const mp_rom_map_elem_t chatesp_plot_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_plot)},
    {MP_ROM_QSTR(MP_QSTR_line), MP_ROM_PTR(&chatesp_plot_line_object)},
};
static MP_DEFINE_CONST_DICT(
    chatesp_plot_globals, chatesp_plot_globals_table);

const mp_obj_module_t chatesp_plot_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&chatesp_plot_globals,
};

MP_REGISTER_MODULE(MP_QSTR_plot, chatesp_plot_module);
