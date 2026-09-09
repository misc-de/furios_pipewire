#!/bin/bash
# Installiert die Umschalt-Infrastruktur. Aendert nichts am laufenden Audiostack -
# nach der Installation ist weiterhin Profil 'standard' aktiv.
set -e
cd "$(dirname "$0")"
sudo mkdir -p /usr/local/share/furios-audio /usr/local/bin
sudo install -m755 audioctl                  /usr/local/bin/audioctl
sudo install -m644 tunnel.conf               /usr/local/share/furios-audio/tunnel.conf
sudo install -m644 furios-pw-tunnel.service  /etc/systemd/user/furios-pw-tunnel.service
sudo install -m644 furios-audio-apply.service /etc/systemd/user/furios-audio-apply.service
mkdir -p /var/lib/furios-audio 2>/dev/null || { sudo mkdir -p /var/lib/furios-audio; sudo chown "$USER:$USER" /var/lib/furios-audio; }
echo standard > /var/lib/furios-audio/profile
systemctl --user daemon-reload
systemctl --user enable furios-audio-apply.service
echo
echo "Fertig. Aktives Profil unveraendert: standard"
echo "Pruefen mit:  audioctl status"
