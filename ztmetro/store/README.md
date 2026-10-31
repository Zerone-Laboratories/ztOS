# App Store (ztstore)

Apps from the repos in `pacman.conf` and from the AUR, in ztMETRO's look,
built on ZrnMetroCore.

**top** lists what is popular and new, **categories** browses AppStream
categories and pacman groups, and **installed** shows updates and your apps.
There is also search across both sources, a details page, and installing,
removing and building from the AUR.

**ztOS apps** lists what the ztOS store offers: built from source at an approved
commit and signed. `zrn store` (zrn-sdk) reads the signed index, trusting only
`/usr/share/zrn/trusted-keys/store.pub`, and checks each package against it. A
revoked version is withdrawn from the app list and `zrn-exec` refuses it.

## Nothing is written into it

| What | Where it comes from |
|---|---|
| repos, and their order | `/etc/pacman.conf` |
| packages, versions, sizes, groups | pacman's sync and local databases, read directly |
| app names, summaries, icons, categories | AppStream (`archlinux-appstream-data`), else the app's `.desktop` file |
| the lists on **top** | `/etc/ztstore/sections` (or `~/.config/ztstore/sections`) |
| AUR lists | the AUR's daily metadata dump, cached for a day |
| AUR search and details | the AUR RPC (v5) |
| "popular in your repos" | pkgstats.archlinux.org, cached for a day |
| updates from the repos | `ztos-update`, the system updater, as the Updates app does |

## How installing works

- **From the repos**: `ztstore-install@USER.service` runs `pacman -S` as root.
  There is no password, because the packages are signed (`50-ztstore.rules`).
- **From the AUR**: `ztstore-aur` fetches the recipe and checks it is made for
  this machine. Then:
  1. What it needs from the repos goes in first.
  2. It is built as you, with `zrnperfd` letting the build use the big cores.
  3. The built package goes in through `ztstore-admin@USER.service`, which asks
     your password on ztMETRO's password sheet (`ztos-polkit-agent`).
- **Removing** also goes through the admin service, and also asks for the password.

The system side lives in `os/overlay`, as the updater's does: `ztstore-helper`,
`ztstore-aur` (`/usr/local/libexec/ztstore/`), the two services, the polkit rule
(local, active session, wheel only). `make install` puts `/etc/ztstore/sections` in place.

The app writes its request to `$XDG_RUNTIME_DIR/ztstore/request`. The services
answer in `/run/ztstore/USER/status` and `log`.

## Building and checking

    make store/ztstore     # on the phone, with the rest of ztmetro
    make check-store       # the parsers, against a made-up pacman layout
    ztstore --render top out.png          # a page as a PNG, no display needed
    ztstore --render detail out.png mpv

## Known limits

- An AUR package that needs other AUR packages stops and says which ones. You
  install those first; it doesn't resolve AUR chains itself.
- The AUR's lists can't filter by architecture: the dump doesn't say. That is
  checked when you install, from the recipe's `.SRCINFO`.
- AUR packages have no icons until they are installed.
- Updates from the repos are whole-system, through `ztos-update`. Arch has no
  partial upgrades.
