#!/bin/bash
# Baut ein .deb aus dem Arbeitsbaum.
#
# Ohne debhelper - dpkg-deb genuegt fuer ein Paket dieser Groesse, und die
# Abhaengigkeitsliste ist von Hand ehrlicher als jede Automatik: sie nennt
# genau die PipeWire-Fassung, gegen die das Plugin gebaut wurde.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)

PLUGIN=poc/spa-droid/build/libspa-droid.so
[ -f "$PLUGIN" ] || { echo "Plugin fehlt - erst bauen: ninja -C poc/spa-droid/build"; exit 1; }

ARCH=$(dpkg --print-architecture)
TRIPLET=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
PWVER=$(pkg-config --modversion libpipewire-0.3 2>/dev/null || echo 1.6.6)
VERSION="0.1.0+git$(git log -1 --format=%cd --date=format:%Y%m%d 2>/dev/null || date +%Y%m%d).$(git rev-parse --short HEAD 2>/dev/null || echo 0)"
PKG="furios-audio-pipewire"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

echo "Paket $PKG $VERSION ($ARCH), gebaut gegen PipeWire $PWVER"

# --- Dateien ---
install -Dm644 "$PLUGIN" "$STAGE/usr/lib/$TRIPLET/spa-0.2/droid/libspa-droid.so"
printf '%s\n' "$PWVER" > "$STAGE/usr/lib/$TRIPLET/spa-0.2/droid/gebaut-gegen"

install -Dm755 audioctl                       "$STAGE/usr/bin/audioctl"
install -Dm755 gui/furios-audio-switch.py     "$STAGE/usr/bin/furios-audio-switch"

install -Dm644 tunnel.conf                    "$STAGE/usr/share/furios-audio/tunnel.conf"
./gen-pipewire-hal-conf.py /usr/share/pipewire/pipewire-droid.conf "$STAGE/tmp-hal.conf" >/dev/null
install -Dm644 "$STAGE/tmp-hal.conf"          "$STAGE/usr/share/furios-audio/pipewire-hal.conf"
rm -f "$STAGE/tmp-hal.conf"

install -Dm644 wireplumber/droid.lua          "$STAGE/usr/share/wireplumber/scripts/monitors/droid.lua"
install -Dm644 wireplumber/50-droid.conf      "$STAGE/usr/share/wireplumber/wireplumber.conf.d/50-droid.conf"
install -Dm644 wireplumber/51-bluez-ofono.conf "$STAGE/usr/share/wireplumber/wireplumber.conf.d/51-bluez-ofono.conf"

install -Dm644 furios-pw-tunnel.service       "$STAGE/usr/lib/systemd/user/furios-pw-tunnel.service"
install -Dm644 furios-audio-apply.service     "$STAGE/usr/lib/systemd/user/furios-audio-apply.service"

install -Dm644 gui/de.furios.audioswitch.desktop \
    "$STAGE/usr/share/applications/de.furios.audioswitch.desktop"
install -Dm644 gui/de.furios.audioswitch.svg \
    "$STAGE/usr/share/icons/hicolor/scalable/apps/de.furios.audioswitch.svg"
install -Dm644 README.md "$STAGE/usr/share/doc/$PKG/README.md"

# --- Steuerdatei ---
mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: misc-de <11610690+misc-de@users.noreply.github.com>
Section: sound
Priority: optional
Depends: pipewire (>= $PWVER), pipewire (<< $(echo "$PWVER" | cut -d. -f1).$(( $(echo "$PWVER" | cut -d. -f2) + 1 ))),
 pipewire-pulse, wireplumber, pulseaudio, libhardware2, libexpat1,
 python3-gi, gir1.2-adw-1, gir1.2-gtk-4.0, pulseaudio-utils
Recommends: libspa-0.2-bluetooth
Description: PipeWire spricht direkt mit dem Android-Audio-HAL
 Ein SPA-Plugin, das den Android-Audio-HAL (libhybris) unmittelbar an PipeWire
 anbindet - ohne PulseAudio dazwischen. Wiedergabe, Aufnahme und Telefonie
 laufen darueber, samt Ohrmuschel- und Lautsprecherumschaltung im Gespraech.
 .
 Dazu audioctl zum Umschalten zwischen dem Auslieferungszustand und dem neuen
 Stack (jederzeit reversibel, mit Sicherheitsnetz) und eine kleine
 GTK4-Oberflaeche dafuer.
 .
 Die Fassungsgrenze auf pipewire ist Absicht: das Plugin wird gegen eine
 bestimmte SPA-Schnittstelle gebaut. Bricht ein Update sie, waere der Ton sonst
 kommentarlos weg.
EOF

cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
# Zustandsverzeichnis; der Umschalter schreibt als Benutzer hinein.
mkdir -p /var/lib/furios-audio
chmod 1777 /var/lib/furios-audio
[ -e /var/lib/furios-audio/profile ] || echo standard > /var/lib/furios-audio/profile
gtk-update-icon-cache -qtf /usr/share/icons/hicolor 2>/dev/null || true
update-desktop-database -q /usr/share/applications 2>/dev/null || true
echo "Installiert. Aktives Profil unveraendert - umschalten mit: audioctl toggle"
EOF
chmod 755 "$STAGE/DEBIAN/postinst"

cat > "$STAGE/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
# Wer beim Entfernen noch auf pw-hal steht, haette hinterher keinen Ton mehr:
# das Plugin verschwindet, die Konfiguration verweist aber weiter darauf.
if [ "$1" = remove ] && [ "$(cat /var/lib/furios-audio/profile 2>/dev/null)" = pw-hal ]; then
    echo "ACHTUNG: Profil pw-hal ist persistent gesetzt." >&2
    echo "         Vor dem Entfernen 'audioctl set standard' ausfuehren," >&2
    echo "         sonst bleibt der Ton nach dem naechsten Neustart weg." >&2
fi
exit 0
EOF
chmod 755 "$STAGE/DEBIAN/prerm"

OUT="$ROOT/packaging/${PKG}_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT" >/dev/null
echo "Fertig: $OUT"
dpkg-deb --info "$OUT" | sed -n '1,12p'
