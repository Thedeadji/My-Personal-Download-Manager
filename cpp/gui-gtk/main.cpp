// MPDM desktop GUI (GTK4 + libadwaita) entry point.
//
// Single instance comes from GApplication (D-Bus): launching MPDM again — from
// the app grid, at login with --background, or by the browser host — forwards
// the command line to the running copy instead of starting a second one.
//
// Dev screenshots (render one surface to PNG, then quit; never touch the real
// instance — pair with MPDM_STATE_DIR to keep the user's queue untouched):
//   --demo                 seed sample downloads
//   --shot <png>           main window
//   --shot-popup <png>     progress window of a file download
//   --shot-stream <png>    progress window of a stream grab
//   --shot-done <png>      progress window after a download completed
//   --shot-prefs <page> <png>   preferences on a page (general, downloads, …)
//   --shot-add <png> / --shot-remove <png>   dialogs
#include <cstring>
#include <string>
#include <vector>

#include "app.hpp"

int main(int argc, char** argv) {
    mpdm::gtk::ShotRequest shot;
    std::vector<char*> args{argv[0]};
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--demo") shot.demo = true;
        else if (a == "--shot") { shot.kind = "main"; shot.out = next(); }
        else if (a == "--shot-popup") { shot.kind = "popup"; shot.out = next(); }
        else if (a == "--shot-stream") { shot.kind = "stream"; shot.out = next(); }
        else if (a == "--shot-done") { shot.kind = "done"; shot.out = next(); }
        else if (a == "--shot-add") { shot.kind = "add"; shot.out = next(); }
        else if (a == "--shot-remove") { shot.kind = "remove"; shot.out = next(); }
        else if (a == "--shot-prefs") { shot.kind = "prefs"; shot.page = next(); shot.out = next(); }
        else args.push_back(argv[i]);
    }
    mpdm::gtk::App app(shot);
    return app.run((int)args.size(), args.data());
}
