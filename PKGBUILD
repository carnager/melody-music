# Maintainer: Rasmus Steinke <rasi at xssn dot at>
pkgname=('melodyd' 'melody-agent' 'melody-airplay' 'melody-tui' 'melody-cli' 'melody-musiclist' 'melody-lrcmatch')
pkgver=1.1
pkgrel=1
arch=('x86_64' 'aarch64')
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
  install -Dm755 "$srcdir/melody-music/bin/melodyd" \
                  "$pkgdir/usr/bin/melodyd"
  install -Dm644 "$srcdir/melody-music/melodyd/melodyd.service" \
                  "$pkgdir/usr/lib/systemd/user/melodyd.service"
}

package_melody-agent() {
  pkgdesc="Remote playback agent for Melody"
  install -Dm755 "$srcdir/melody-music/bin/melody-agent" \
                  "$pkgdir/usr/bin/melody-agent"
}

package_melody-airplay() {
  pkgdesc="AirPlay/CoreAudio playback agent for Melody"
  install -Dm755 "$srcdir/melody-music/bin/melody-airplay" \
                  "$pkgdir/usr/bin/melody-airplay"
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

package_melody-musiclist() {
  pkgdesc="Static music list exporter for Melody"
  optdepends=('melodyd: local daemon')
  install -Dm755 "$srcdir/melody-music/bin/melody-musiclist" \
                  "$pkgdir/usr/bin/melody-musiclist"
}

package_melody-lrcmatch() {
  pkgdesc="Offline lyrics matcher for Melody (lrclib database dump + NetEase)"
  depends=('melodyd')
  install -Dm755 "$srcdir/melody-music/bin/melody-lrcmatch" \
                  "$pkgdir/usr/bin/melody-lrcmatch"
}
