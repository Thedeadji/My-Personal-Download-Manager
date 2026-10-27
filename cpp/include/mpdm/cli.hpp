// Command-line interface — port of mpdm/cli.py (get / grab / clean).
#pragma once

namespace mpdm {

// Parse argv and run the requested subcommand. Returns a process exit code.
int run_cli(int argc, char** argv);

} // namespace mpdm
