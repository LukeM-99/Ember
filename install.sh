#!/bin/sh
# install.sh: apt deps, build, then link Ember, its MPD config and its launcher entry into ~.
set -e
cd "$(dirname "$(readlink -f "$0")")"

# Someone who already runs MPD keeps their setup: their system service, and their config.
had_mpd=; dpkg -s mpd >/dev/null 2>&1 && had_mpd=1
conf=~/.config/mpd/mpd.conf
own_conf=; { [ -e "$conf" ] || [ -L "$conf" ]; } && [ "$(readlink -f "$conf")" != "$PWD/config/mpd.conf" ] && own_conf=1
[ -e ~/.mpdconf ] && own_conf=1

sudo apt update  # stale security indexes 404 on superseded packages
if [ -z "$had_mpd" ]; then
  # mpd's postinst starts the system service, which fails without systemd (/etc/mpd.conf has no
  # pid_file); policy-rc.d stops it starting at all, and we run MPD as the user instead.
  # --no-install-recommends: mpd's recommends pull in avahi
  [ -e /usr/sbin/policy-rc.d ] || { printf '#!/bin/sh\nexit 101\n' | sudo tee /usr/sbin/policy-rc.d >/dev/null
    sudo chmod +x /usr/sbin/policy-rc.d; trap 'sudo rm -f /usr/sbin/policy-rc.d' EXIT; }
  sudo apt install -y --no-install-recommends mpd
  sudo update-rc.d mpd disable
fi
sudo apt install -y build-essential pkg-config libgtk-3-dev libmpdclient-dev curl  # curl: online covers

pkg-config --atleast-version=2.74 glib-2.0 && pkg-config --atleast-version=2.20 libmpdclient ||
  { echo "Ember needs Debian 12 (bookworm) or Devuan 5 (daedalus), or newer." >&2; exit 1; }

make

# Symlinks into ~ (as Kindling does): keep the clone where it is. Anything already there that
# isn't a symlink goes to ~/.dotfiles_backup/<date>/ first.
backup=~/.dotfiles_backup/$(date +%F_%H%M%S)
link() {
  if [ -e "$2" ] && [ ! -L "$2" ]; then mkdir -p "$backup"; mv "$2" "$backup/"; fi
  mkdir -p "$(dirname "$2")"
  ln -sfn "$1" "$2"
}
if [ -z "$own_conf" ]; then
  mkdir -p ~/.local/share/mpd/playlists
  link "$PWD/config/mpd.conf" "$conf"
fi
link "$PWD/ember" ~/.local/bin/ember
link "$PWD/data/ember.desktop" ~/.local/share/applications/ember.desktop  # rofi's app list
link "$PWD/data/ember.svg" ~/.local/share/icons/hicolor/scalable/apps/ember.svg
# an icon cache made before the icon was linked would hide it from GTK, rofi and the taskbar
[ -e ~/.local/share/icons/hicolor/icon-theme.cache ] && gtk-update-icon-cache -qtf ~/.local/share/icons/hicolor

echo "Done. Start Ember from the app launcher, or run ember (MPD starts itself the first time)."
[ -z "$own_conf" ] || echo "Your MPD config was left alone. Point Ember at your MPD with MPD_HOST, e.g. MPD_HOST=localhost ember (or the path of its socket); see the README."
