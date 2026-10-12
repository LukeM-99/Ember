#!/bin/sh
# install.sh: apt deps, build, then link Ember, its MPD config and its launcher entry into ~.
set -e
cd "$(dirname "$(readlink -f "$0")")"

# mpd's postinst starts the system service, which fails without systemd (/etc/mpd.conf has no
# pid_file); policy-rc.d stops it starting at all, and we run MPD as the user instead.
# --no-install-recommends: mpd's recommends pull in avahi
[ -e /usr/sbin/policy-rc.d ] || { printf '#!/bin/sh\nexit 101\n' | sudo tee /usr/sbin/policy-rc.d >/dev/null
  sudo chmod +x /usr/sbin/policy-rc.d; trap 'sudo rm -f /usr/sbin/policy-rc.d' EXIT; }
sudo apt update  # stale security indexes 404 on superseded packages
sudo apt install -y --no-install-recommends mpd
sudo update-rc.d mpd disable
sudo apt install -y build-essential pkg-config libgtk-3-dev libmpdclient-dev curl  # curl: online covers

make

# Symlinks into ~ (as Kindling does): keep the clone where it is. Anything already there that
# isn't a symlink goes to ~/.dotfiles_backup/<date>/ first.
backup=~/.dotfiles_backup/$(date +%F_%H%M%S)
link() {
  if [ -e "$2" ] && [ ! -L "$2" ]; then mkdir -p "$backup"; mv "$2" "$backup/"; fi
  mkdir -p "$(dirname "$2")"
  ln -sfn "$1" "$2"
}
mkdir -p ~/.local/share/mpd/playlists
link "$PWD/config/mpd.conf" ~/.config/mpd/mpd.conf
link "$PWD/ember" ~/.local/bin/ember
link "$PWD/data/ember.desktop" ~/.local/share/applications/ember.desktop  # rofi's app list
link "$PWD/data/ember.svg" ~/.local/share/icons/hicolor/scalable/apps/ember.svg
# an icon cache made before the icon was linked would hide it from GTK, rofi and the taskbar
[ -e ~/.local/share/icons/hicolor/icon-theme.cache ] && gtk-update-icon-cache -qtf ~/.local/share/icons/hicolor

echo "Done. Start Ember from the app launcher, or run ember (MPD starts itself the first time)."
