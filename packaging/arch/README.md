# Arch Linux development package

`melody-git` builds GitHub HEAD and includes the daemon and Linux command-line
clients. The repository-root `PKGBUILD` remains the tagged split-package recipe.

Keep this directory's `PKGBUILD` and `melody.install` synchronized with the AUR
`melody-git` repository. Build with `makepkg`; the check phase runs `go test ./...`.
After updating sources, regenerate AUR metadata with `makepkg --printsrcinfo`.

The package installs `/usr/lib/systemd/user/melodyd.service`. As your regular
user, configure it before starting the service:

```sh
melodyd setup
systemctl --user daemon-reload
systemctl --user enable --now melodyd.service
```

Installation does not enable or start services. The install/upgrade messages
explain setup and restarting an existing service. For an unattended server,
an administrator can optionally enable lingering for the service's user.
