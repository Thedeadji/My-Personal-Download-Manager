// Native-messaging host for the browser extension — port of mpdm/native_host.py.
// Protocol: a 4-byte little-endian length prefix followed by that many bytes of
// UTF-8 JSON, on stdin/stdout. This C++ host runs downloads headlessly (the
// IDM-style progress popup is a GUI-phase feature).
#pragma once
#include <istream>
#include <optional>
#include <ostream>
#include <string>

namespace mpdm {

// Framing (testable on any stream).
void write_frame(std::ostream& out, const std::string& json_body);
std::optional<std::string> read_frame(std::istream& in);  // nullopt on EOF/short read

// True if a job is an HLS/DASH stream (route through the grabber).
bool is_stream_job(const std::string& url, bool stream_flag);

// Entry point: run the read/dispatch loop (or --selftest). Returns an exit code.
int run_native_host(int argc, char** argv);

} // namespace mpdm
