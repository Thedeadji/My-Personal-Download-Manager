// Native-messaging host executable: browsers launch this via install_host.
// Keeps stdout reserved for the 4-byte-framed protocol (the CLI uses stdout for
// human output, so the host is its own binary).
#include "mpdm/native_host.hpp"

int main(int argc, char** argv) { return mpdm::run_native_host(argc, argv); }
