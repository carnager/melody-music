# Arch Linux development package

The `melody-git` package base builds GitHub HEAD into separate packages:

- `melodyd-git`: daemon, user service, and setup messages
- `melody-agent-git`: remote playback agent
- `melody-tui-git`, `melody-cli-git`: terminal clients
- `melody-musiclist-git`, `melody-lrcmatch-git`: export and lyrics tools
- `melody-watcher-git`, `melody-rofi-git`: filesystem watcher and menu client

Clients do not depend on the daemon. The optional `melody-git` metapackage
installs all components and preserves the previous bundled package's upgrade
path. Each component conflicts only with its corresponding stable package.
The repository-root `PKGBUILD` remains the tagged split-package recipe.

Keep this directory's `PKGBUILD` and `melody.install` synchronized with the AUR
`melody-git` repository. Build with `makepkg`; the check phase runs `go test ./...`.
After updating sources, regenerate AUR metadata with `makepkg --printsrcinfo`.

Only `melodyd-git` installs `/usr/lib/systemd/user/melodyd.service`. As your regular
user, configure it before starting the service:

```sh
melodyd setup
systemctl --user daemon-reload
systemctl --user enable --now melodyd.service
```

Installation does not enable or start services. The install/upgrade messages
explain setup and restarting an existing service. For an unattended server,
an administrator can optionally enable lingering for the service's user.
