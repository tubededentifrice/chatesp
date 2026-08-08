#pragma once

#include <port/mpconfigport_common.h>

void chatesp_micropython_vm_hook(void);

#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)
#define MICROPY_ENABLE_COMPILER (1)
#define MICROPY_ENABLE_GC (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT (0)
#define MICROPY_ENABLE_FINALISER (0)
#define MICROPY_ENABLE_SOURCE_LINE (1)
#define MICROPY_ENABLE_VM_ABORT (0)
#define MICROPY_FLOAT_IMPL (MICROPY_FLOAT_IMPL_DOUBLE)
#define MICROPY_GCREGS_SETJMP (1)
#define MICROPY_LONGINT_IMPL (MICROPY_LONGINT_IMPL_LONGLONG)
#define MICROPY_NLR_SETJMP (1)
#define MICROPY_PERSISTENT_CODE_LOAD (0)
#define MICROPY_PY_ARRAY (1)
#define MICROPY_PY_BUILTINS_COMPILE (0)
#define MICROPY_PY_BUILTINS_EVAL_EXEC (0)
#define MICROPY_PY_BUILTINS_EXECFILE (0)
#define MICROPY_PY_CMATH (1)
#define MICROPY_PY_COLLECTIONS (0)
#define MICROPY_PY_GC (0)
#define MICROPY_PY_IO (0)
#define MICROPY_PY_MATH (1)
#define MICROPY_PY_MICROPYTHON (0)
#define MICROPY_PY_OS (0)
#define MICROPY_PY_PLATFORM (0)
#define MICROPY_PY_SYS (0)
#define MICROPY_STACK_CHECK (1)
#define MICROPY_VM_HOOK_LOOP chatesp_micropython_vm_hook();
#define MICROPY_VM_HOOK_RETURN chatesp_micropython_vm_hook();
#define MICROPY_GC_HOOK_LOOP(i) \
    do { \
        if (((i) & 31) == 0) { \
            chatesp_micropython_vm_hook(); \
        } \
    } while (0)
