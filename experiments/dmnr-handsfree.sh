#!/bin/bash
# Versuch: MediaTeks Freisprech-Echounterdrueckung (DMNR) im Anruf einschalten.
#
# Hintergrund
# -----------
# Die Abstimmungsdatei des Herstellers sagt auf diesem Geraet:
#
#   MTK_DUAL_MIC_SUPPORT         yes   zwei Mikrofone sind da
#   MTK_HANDSFREE_DMNR_SUPPORT   yes   der Chip kann Freisprech-DMNR
#   MTK_INCALL_HANDSFREE_DMNR    no    im Anruf ist sie ABGESCHALTET
#   MTK_VOIP_HANDSFREE_DMNR      no
#   MTK_VOIP_NORMAL_DMNR         no
#
# DMNR ist MediaTeks Zweimikrofonverfahren gegen Stoergeraeusche und Echo.
# Ist es aus, hoert die Gegenseite sich selbst - besonders beim Freisprechen.
#
# Warum ein Bind-Mount
# --------------------
# /android/vendor ist schreibgeschuetzt eingehaengt und per dm-verity
# abgesichert. Daran zu schrauben macht das Geraet im schlimmsten Fall
# unstartbar. Ein Bind-Mount legt stattdessen eine geaenderte Kopie ueber die
# Datei - die Partition bleibt unberuehrt, und ein Neustart raeumt alles weg.
#
# Wirksam wird es erst, wenn der HAL die Datei neu liest, also beim naechsten
# Start des Audiostacks (das erledigt das Skript).
set -e

ORIG=/android/vendor/etc/audio_param/AudioParamOptions.xml
COPY=/var/lib/furios-audio/AudioParamOptions.dmnr.xml

show() {
    printf 'Datei:  %s\n' "$ORIG"
    if mountpoint -q "$ORIG" 2>/dev/null || grep -q " $ORIG " /proc/mounts 2>/dev/null; then
        printf 'Zustand: geaenderte Kopie liegt darueber\n'
    else
        printf 'Zustand: Original des Herstellers\n'
    fi
    printf 'Aktuelle Werte:\n'
    grep -oE '<Param name="(MTK_INCALL_HANDSFREE_DMNR|MTK_VOIP_HANDSFREE_DMNR|MTK_VOIP_NORMAL_DMNR|MTK_HANDSFREE_DMNR_SUPPORT|MTK_DUAL_MIC_SUPPORT)" value="[^"]*"' "$ORIG" \
        | sed 's/<Param name="/  /; s/" value="/ = /; s/"$//'
}

case "${1:-status}" in
status) show ;;

an)
    [ -r "$ORIG" ] || { echo "Abstimmungsdatei nicht lesbar - falsches Geraet?" >&2; exit 1; }
    mkdir -p "$(dirname "$COPY")"
    sed -e 's/<Param name="MTK_INCALL_HANDSFREE_DMNR" value="no"/<Param name="MTK_INCALL_HANDSFREE_DMNR" value="yes"/' \
        -e 's/<Param name="MTK_VOIP_HANDSFREE_DMNR" value="no"/<Param name="MTK_VOIP_HANDSFREE_DMNR" value="yes"/' \
        -e 's/<Param name="MTK_VOIP_NORMAL_DMNR" value="no"/<Param name="MTK_VOIP_NORMAL_DMNR" value="yes"/' \
        "$ORIG" > "$COPY"
    if cmp -s "$ORIG" "$COPY"; then
        echo "Nichts zu aendern - die Schalter stehen nicht auf 'no'." >&2
        rm -f "$COPY"; exit 1
    fi
    sudo mount --bind "$COPY" "$ORIG"
    echo "Geaenderte Kopie eingehaengt. Audiostack neu starten, damit der HAL sie liest:"
    audioctl set standard >/dev/null 2>&1 || true
    echo
    show
    echo
    echo "Jetzt anrufen und die Gegenseite fragen, ob das Echo weg ist."
    echo "Zurueck: $0 aus   (oder einfach neu starten)"
    ;;

aus)
    if grep -q " $ORIG " /proc/mounts 2>/dev/null; then
        sudo umount "$ORIG"
        echo "Original wiederhergestellt."
    else
        echo "Es lag nichts darueber."
    fi
    audioctl set standard >/dev/null 2>&1 || true
    show
    ;;

*) echo "Aufruf: $0 [status|an|aus]" >&2; exit 1 ;;
esac
