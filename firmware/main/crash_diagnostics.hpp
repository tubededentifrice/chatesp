#pragma once

#include "chatesp/crash_trace.hpp"

namespace chatesp {
namespace crash_diagnostics {

void initialize();
void mark(runtime::CrashEvent event);
void heartbeat();

}  // namespace crash_diagnostics
}  // namespace chatesp
