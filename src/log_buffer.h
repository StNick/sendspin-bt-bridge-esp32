// Bounded in-memory copy of the ESP log output for the web UI's log view
// (the role sendspin-bt-bridge's _RingLogHandler plays for its SSE log endpoint).
#pragma once

#include <cstddef>
#include <string>

namespace log_buffer {

/// Installs the log hook. Output still goes to the serial console as before.
void init(size_t capacity_bytes);

/// Returns the buffered log text, oldest line first.
std::string dump();

}  // namespace log_buffer
