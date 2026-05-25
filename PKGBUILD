# Maintainer: Rasmus Steinke <rasi at xssn dot at>
pkgname=('melodyd' 'melody-agent' 'melody-tui' 'melody-cli' 'melody-rofi' 'melody-musiclist' 'melody-lrcmatch')
pkgver=0.13.0
pkgrel=1
arch=('x86_64')
url="https://github.com/carnager/melody-music"
license=('MIT')
makedepends=('go')
source=("git+https://github.com/carnager/melody-music.git#tag=v${pkgver}")
sha256sums=('SKIP')

build() {
  cd "$srcdir/melody-music"
  export GOMODCACHE="$srcdir/gomodcache"
  export GOCACHE="$srcdir/gobuild"
  export GOSUMDB=off
  ./build
  chmod -R u+w "$srcdir/gomodcache" 2>/dev/null || true
}

package_melodyd() {
  pkgdesc="Melody music server daemon"
  depends=('mpv')
  install -Dm755 "$srcdir/melody-music/bin/melodyd" \
                  "$pkgdir/usr/bin/melodyd"
  install -Dm644 "$srcdir/melody-music/melodyd/melodyd.service" \
                  "$pkgdir/usr/lib/systemd/user/melodyd.service"
}

package_melody-agent() {
  pkgdesc="Remote playback agent for Melody"
  depends=('mpv')
  install -Dm755 "$srcdir/melody-music/bin/melody-agent" \
                  "$pkgdir/usr/bin/melody-agent"
}

package_melody-tui() {
  pkgdesc="Terminal UI for Melody"
  depends=('melodyd')
  install -Dm755 "$srcdir/melody-music/bin/melody-tui" \
                  "$pkgdir/usr/bin/melody-tui"
}

package_melody-cli() {
  pkgdesc="Command-line client for Melody"
  optdepends=('melodyd: local daemon')
  install -Dm755 "$srcdir/melody-music/bin/melody-cli" \
                  "$pkgdir/usr/bin/melody-cli"
}

package_melody-rofi() {
  pkgdesc="Rofi client for Melody"
  depends=('rofi')
  optdepends=('melodyd: local daemon')
  install -Dm755 "$srcdir/melody-music/bin/melody-rofi" \
                  "$pkgdir/usr/bin/melody-rofi"
}

package_melody-musiclist() {
  pkgdesc="Static music list exporter for Melody"
  optdepends=('melodyd: local daemon')
  install -Dm755 "$srcdir/melody-music/bin/melody-musiclist" \
                  "$pkgdir/usr/bin/melody-musiclist"
}

package_melody-lrcmatch() {
  pkgdesc="Offline lyrics matcher for Melody (uses lrclib database dump)"
  depends=('melodyd')
  install -Dm755 "$srcdir/melody-music/bin/melody-lrcmatch" \
                  "$pkgdir/usr/bin/melody-lrcmatch"
}
