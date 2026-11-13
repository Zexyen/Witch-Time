#!/bin/sh
# Witch Time.app beside the launcher it was built with (launcher/CMakeLists.txt, macOS): Finder and the Dock start a
# bundle, not a bare program. The program inside is a copy of build/witch-time, which finds the Witch Time folder from
# the bundle's place (Paths::find), so the bundle stays in build/; drag it to the Dock, or make an alias of it.
#     make_app.sh <build/witch-time> <Info.plist> <icon.png> <build/Witch Time.app>
set -e
exe=$1 plist=$2 icon=$3 app=$4
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$exe" "$app/Contents/MacOS/witch-time"
cp "$plist" "$app/Contents/Info.plist"
set="$app/Contents/Resources/witch-time.iconset"
mkdir -p "$set"
for s in 16 32 128 256; do
	sips -z $s $s "$icon" --out "$set/icon_${s}x${s}.png" >/dev/null
	d=$((s * 2))
	[ $d -le 256 ] && sips -z $d $d "$icon" --out "$set/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$set" -o "$app/Contents/Resources/witch-time.icns"
rm -rf "$set"
codesign --force --sign - "$app" >/dev/null 2>&1 || true    # ad hoc, as the linker signed the program itself
