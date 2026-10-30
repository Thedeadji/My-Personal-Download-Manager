#!/usr/bin/env bash
# Package MPDM as one self-contained AppImage (desktop app + browser host + CLI).
#
# Usage:
#   packaging/linux/build-appimage.sh qt|gtk <build-dir> [out-dir]
#
# <build-dir> is a CMake tree already built with exactly that front-end enabled
# (-DMPDM_BUILD_GUI=ON for qt, -DMPDM_BUILD_GTK_GUI=ON for gtk). The first run
# downloads linuxdeploy and its Qt or GTK plugin into <build-dir>/appimage-tools.
#
# Output: <out-dir>/MPDM-Qt-<version>-x86_64.AppImage or MPDM-GTK-<version>-x86_64.AppImage
# (version from the build's MPDM_VERSION). Libraries come from the build machine,
# so the AppImage runs on distros with the same or a newer glibc.
set -euo pipefail

flavor="${1:?usage: build-appimage.sh qt|gtk <build-dir> [out-dir]}"
build="$(realpath "${2:?usage: build-appimage.sh qt|gtk <build-dir> [out-dir]}")"
out="$(realpath -m "${3:-$build}")"
here="$(dirname "$(readlink -f "$0")")"
version="$(sed -n 's/^MPDM_VERSION:STRING=//p' "$build/CMakeCache.txt")"

case "$flavor" in
    qt)  gui=mpdm_gui; desktop=mpdm.desktop; name=MPDM-Qt ;;
    gtk) gui=mpdm_gtk; desktop=com.mypersonaldownloadmanager.MPDM.desktop; name=MPDM-GTK ;;
    *)   echo "build-appimage.sh: flavor must be qt or gtk" >&2; exit 2 ;;
esac
[ -x "$build/$([ "$flavor" = qt ] && echo gui || echo gui-gtk)/$gui" ] || {
    echo "build-appimage.sh: $gui isn't built in $build" >&2; exit 1; }

# ---- tools ------------------------------------------------------------------
tools="$build/appimage-tools"
mkdir -p "$tools"
fetch() {
    [ -x "$tools/$1" ] && return
    curl -fsSL --retry 3 -o "$tools/$1" "$2"
    chmod +x "$tools/$1"
}
fetch linuxdeploy-x86_64.AppImage \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
if [ "$flavor" = qt ]; then
    fetch linuxdeploy-plugin-qt-x86_64.AppImage \
        https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
else
    fetch linuxdeploy-plugin-gtk.sh \
        https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh
fi
export PATH="$tools:$PATH"             # linuxdeploy finds its plugins on PATH
export APPIMAGE_EXTRACT_AND_RUN=1      # CI runners and containers have no FUSE
linuxdeploy="$tools/linuxdeploy-x86_64.AppImage"

# ---- AppDir -----------------------------------------------------------------
appdir="$build/AppDir"
rm -rf "$appdir"
DESTDIR="$appdir" cmake --install "$build" --prefix /usr
sed "s|@GUI_BIN@|$gui|g" "$here/AppRun" > "$build/AppRun"
chmod +x "$build/AppRun"

args=(--appdir "$appdir"
      --desktop-file "$appdir/usr/share/applications/$desktop"
      --icon-file "$appdir/usr/share/icons/hicolor/256x256/apps/mpdm.png"
      --custom-apprun "$build/AppRun")
for exe in "$gui" mpdm_host mpdm; do args+=(--executable "$appdir/usr/bin/$exe"); done

mkdir -p "$out"
export VERSION="$version"
export OUTPUT="$name-$version-x86_64.AppImage"

if [ "$flavor" = qt ]; then
    export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
    export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so"   # native Wayland as well as X11
    (cd "$out" && "$linuxdeploy" "${args[@]}" --plugin qt --output appimage)
else
    # Adwaita's symbolic icons, so the UI is complete on desktops that lack them.
    if [ -d /usr/share/icons/Adwaita ]; then
        mkdir -p "$appdir/usr/share/icons"
        cp -a /usr/share/icons/Adwaita "$appdir/usr/share/icons/"
    fi
    export DEPLOY_GTK_VERSION=4
    "$linuxdeploy" "${args[@]}" --plugin gtk
    # The GTK plugin's hook forces GTK_THEME and the X11 backend; both break
    # libadwaita (dark style, accent colors, Wayland). Keep only its paths.
    hook="$appdir/apprun-hooks/linuxdeploy-plugin-gtk.sh"
    { echo '#!/usr/bin/env bash'
      grep -E '^export ' "$hook" | grep -vE '^export (GTK_THEME|GDK_BACKEND)='
    } > "$hook.new"
    mv "$hook.new" "$hook"
    (cd "$out" && "$linuxdeploy" --appdir "$appdir" --output appimage)
fi

echo "AppImage: $out/$OUTPUT"
