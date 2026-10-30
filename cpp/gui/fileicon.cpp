#include "fileicon.hpp"

#include <QApplication>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QMimeDatabase>
#include <QPainter>
#include <QPainterPath>

#include <string>

#include "mpdm/categories.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commoncontrols.h>
#endif

namespace mpdm::gui {

static QString extOf(const QString& name) {
    int dot = name.lastIndexOf('.');
    return dot >= 0 && dot < name.size() - 1 ? name.mid(dot + 1).toLower() : QString();
}

#ifdef _WIN32
// The shell's icon for an extension, without needing the file to exist
// (SHGFI_USEFILEATTRIBUTES), taken from the 48px "extra large" image list.
static QPixmap shellIconForExt(const QString& ext) {
    std::wstring probe = L"x." + ext.toStdWString();
    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                        SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES))
        return {};
    IImageList* list = nullptr;
    if (FAILED(SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(&list))) || !list) return {};
    HICON h = nullptr;
    list->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &h);
    list->Release();
    if (!h) return {};
    QPixmap pm = QPixmap::fromImage(QImage::fromHICON(h));
    DestroyIcon(h);
    return pm;
}
#endif

// Icon from the OS for this file type (or, for programs/shortcuts that exist on
// disk, the file's own icon). Null if the OS has nothing better than a blank.
static QPixmap systemIcon(const QString& name, const QString& path, int px) {
    const QString ext = extOf(name);
    static const QStringList ownIcon = {"exe", "ico", "lnk", "url", "appimage"};
    if (!path.isEmpty() && ownIcon.contains(ext) && QFileInfo::exists(path)) {
        QIcon ic = QFileIconProvider().icon(QFileInfo(path));
        if (!ic.isNull()) return ic.pixmap(px, px);
    }
#ifdef _WIN32
    if (!ext.isEmpty()) {
        QPixmap pm = shellIconForExt(ext);
        if (!pm.isNull()) return pm;
    }
    return {};
#else
    QMimeType mt = QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    QIcon ic = QIcon::fromTheme(mt.iconName(), QIcon::fromTheme(mt.genericIconName()));
    return ic.isNull() ? QPixmap() : ic.pixmap(px, px);
#endif
}

// Fallback: a chamfered page with the extension, tinted by category.
static QPixmap badgeIcon(const QString& name, int px, const Palette& P) {
    const std::string cat = mpdm::classify(name.toStdString());
    QColor c = cat == "Video" ? P.cyan : cat == "Audio" ? P.violet : cat == "Documents" ? P.amber
             : cat == "Archives" ? P.green : cat == "Programs" ? P.red : P.text3;
    qreal dpr = qApp->devicePixelRatio();
    QPixmap pm(QSize(px, px) * dpr); pm.setDevicePixelRatio(dpr); pm.fill(Qt::transparent);
    QPainter g(&pm);
    g.setRenderHint(QPainter::Antialiasing, true);
    const qreal w = px * 0.74, h = px * 0.92, x = (px - w) / 2, y = (px - h) / 2, cut = px * 0.24;
    QPainterPath page;
    page.moveTo(x, y); page.lineTo(x + w - cut, y); page.lineTo(x + w, y + cut);
    page.lineTo(x + w, y + h); page.lineTo(x, y + h); page.closeSubpath();
    g.fillPath(page, P.panelHi);
    g.setPen(QPen(c, std::max(1.0, px / 24.0)));
    g.drawPath(page);
    QString label = extOf(name).toUpper().left(4);
    if (!label.isEmpty()) {
        QFont f("JetBrains Mono");
        f.setPixelSize(std::max(6, int(px * (label.size() > 3 ? 0.20 : 0.25))));
        f.setBold(true);
        g.setFont(f);
        g.setPen(c);
        g.drawText(QRectF(x, y + h * 0.45, w, h * 0.45), Qt::AlignCenter, label);
    }
    g.end();
    return pm;
}

QPixmap fileIconFor(const QString& name, const QString& path, int size, const Palette& P) {
    static QHash<QString, QPixmap> cache;
    const QString ext = extOf(name);
    const qreal dpr = qApp->devicePixelRatio();
    const int px = int(size * dpr);
    // Programs that exist on disk get their own icon (keyed by path), others by type.
    const bool own = !path.isEmpty() && (ext == "exe" || ext == "ico" || ext == "lnk" || ext == "url" ||
                                          ext == "appimage") && QFileInfo::exists(path);
    const QString key = (own ? "p:" + path : "e:" + ext) + QString(":%1:%2").arg(size).arg(P.dark);
    if (auto it = cache.constFind(key); it != cache.constEnd()) return *it;

    QPixmap pm = systemIcon(name, own ? path : QString(), px);
    if (!pm.isNull()) {
        pm = pm.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pm.setDevicePixelRatio(dpr);
    } else {
        pm = badgeIcon(name, size, P);
    }
    cache.insert(key, pm);
    return pm;
}

}  // namespace mpdm::gui
