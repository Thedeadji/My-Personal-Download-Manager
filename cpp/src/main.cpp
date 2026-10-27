// MyPersonalDownloadManager — C++ port entry point.
// Thin wrapper over the CLI (get / grab / clean / probe); see cli.cpp.

#include "mpdm/cli.hpp"

int main(int argc, char** argv) { return mpdm::run_cli(argc, argv); }
