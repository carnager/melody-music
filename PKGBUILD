# Maintainer: Rasmus Steinke <rasi at xssn dot at>
pkgname=('melodyd' 'melody-agent' 'melody-tui' 'melodyc' 'melody-rofi' 'melody-musiclist')
pkgver=0.5.0
pkgrel=1
arch=('x86_64')
url="https://github.com/carnager/melody"
license=('MIT')
makedepends=('go')
source=("git+https://github.com/carnager/melody.git#tag=${pkgver}")
sha256sums=('SKIP')

build() {
  cd "$srcdir/melody"
  export GOMODCACHE="$srcdir/gomodcache"
  export GOCACHE="$srcdir/gobuild"
  export GOSUMDB=off
  ./build
  chmod -R u+w "$srcdir/gomodcache" 2>/dev/null || true
}

package_melodyd() {
  pkgdesc="Melody daemon for Navidrome/mpv"
  depends=('mpv')
  install -Dm755 "$srcdir/melody/bin/melodyd" \
                  "$pkgdir/usr/bin/melodyd"
  install -Dm644 "$srcdir/melody/melodyd/melodyd.service" \
                  "$pkgdir/usr/lib/systemd/user/melodyd.service"
}

package_melody-agent() {
  pkgdesc="Remote playback agent for Melody"
  depends=('mpv')
  install -Dm755 "$srcdir/melody/bin/melody-agent" \
                  "$pkgdir/usr/bin/melody-agent"
}

package_melody-tui() {
  pkgdesc="Terminal UI for Melody"
  depends=('melodyd')
  install -Dm755 "$srcdir/melody/bin/melody-tui" \
                  "$pkgdir/usr/bin/melody-tui"
}

package_melodyc() {
  pkgdesc="CLI client for Melody"
  optdepends=('melodyd: local daemon')
  install -Dm755 "$srcdir/melody/bin/melodyc" \
                  "$pkgdir/usr/bin/melodyc"
}

package_melody-rofi() {
  pkgdesc="Rofi client for Melody"
  depends=('rofi')
  optdepends=('melodyd: local daemon')
  install -Dm755 "$srcdir/melody/bin/melody-rofi" \
                  "$pkgdir/usr/bin/melody-rofi"
}

package_melody-musiclist() {
  pkgdesc="Static music list exporter for Melody"
  optdepends=('melodyd: local daemon')
  install -Dm755 "$srcdir/melody/bin/melody-musiclist" \
                  "$pkgdir/usr/bin/melody-musiclist"
}
