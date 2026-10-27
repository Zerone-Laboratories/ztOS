# ztMETRO logo update

## Source changes

- Navigation bar defaults to the two-shard ztMETRO mark. Cairo draws its fixed polygon paths into the existing nav texture; there is no per-frame SVG parse.
- Settings has a navigation bar page with five selectable preview tiles: logo, ZRN, Start square, four panes, and circle. `nav_center_icon` is saved in `~/.config/ztmetro/config`; the settings app sends the existing SIGHUP reload for immediate application.
- First unlock in each user runtime draws two transparent white SVG shards on plain black. Each shard keeps its SVG bounds and proportions, uses the Start tile in/out transform, and staggers by 51 ms. The sequence holds 600 ms. Input skips it. A marker in `$XDG_RUNTIME_DIR` prevents replay after a shell restart during the same boot. Slow motion scales the sequence.

## Phone deployment and evidence

The phone build before the correction compiled both ztMETRO and Settings successfully. It was installed with `/usr/local/bin/ztmetro.logo-backup` retained, and greetd respawned ztMETRO. The phone reported Mali-G72 MC18 (Panfrost) at 360x740.

The first phone capture showed the nav logo, but its splash used accent-filled square tile faces. The user correction required plain black and no tile squares. The source now fixes that presentation and preserves the SVG's actual polygon bounds.

The correction could not be built, deployed, or verified on the phone in this run: the active sandbox rejected the phone controller's network socket with `PermissionError: [Errno 1] Operation not permitted`. Local syntax compilation also could not run because the workspace lacks the `wlroots-0.20` development package. Do not treat the earlier splash timing or frames as proof of the corrected design.

Available PNG evidence, opened and inspected:

- [Nav bar and Start screen](evidence-nav-default.png): logo visible; a shell restart notice remains at the top.
- [App list](evidence-app-list.png): captured while locating Settings.

## Verification still required on phone

After phone access returns, build and deploy the current `ztmetro.c`, preserving `/usr/local/bin/ztmetro.logo-backup`. Check `pgrep -af ztos-fsck` before restarting ztMETRO. Capture and inspect the settings picker, change to another icon and back, and record ~10 fps unlock frames showing the transparent shards in, holding, leaving, then Start tiles entering. Record corrected sequence timing and verify tap skip and replay prevention after shell restart. Do not reboot the phone during filesystem recovery work.
