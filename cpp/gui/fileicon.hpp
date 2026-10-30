// Icons for downloaded files, matched to the file's type: the same icon the OS
// shows for it (Explorer on Windows, the icon theme on Linux). Falls back to a
// drawn badge with the extension in its category color.
#pragma once
#include <QPixmap>
#include <QString>

#include "theme.hpp"

namespace mpdm::gui {

// `name` is the file name (its extension picks the icon); `path` is the file on
// disk if it exists (finished programs then show their own icon); `size` is the
// logical size in pixels. Results are cached.
QPixmap fileIconFor(const QString& name, const QString& path, int size, const Palette& P);

}  // namespace mpdm::gui
