# Files app: install XAPK / APKS / APKM bundles

Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Build on the phone in /home/zerone/ztmetro-phase2 (push only the
files you change), deploy with backups. Other agents are working in ztmetro/browser/ (Superscan)
and on Waydroid health / Bluetooth: don't touch those areas; don't restart Waydroid.

User: "build the ability to install XAPKs into the file manager".
Today .apk files in Files (ztmetro/files) offer "Install with ztOS runtime" / "Install with
Waydroid" and call `ztos-android install APK --runtime atl|waydroid`
(os/overlay/usr/local/bin/ztos-android).

Deliver:
1. `ztos-android install` accepts .xapk (zip: manifest.json, base + split APKs, optional
   Android/obb/...), .apks (SAI/bundletool: *.apk splits, toc.pb/meta) and .apkm (APKMirror,
   unencrypted zip of splits; detect and report the encrypted kind). Parse manifest.json
   (package_name, split_apks[], expansions[]), pick splits for this device (arm64-v8a, xxxhdpi
   ~560-576 dpi, user's language + en), extract to a temp dir under ~/.cache, clean up after.
   - Waydroid: install base+splits atomically (`pm install-create`, `install-write` each split,
     `install-commit`; or `adb install-multiple` equivalent through `waydroid shell`), copy OBB
     files to /sdcard/Android/obb/<pkg>/ inside the container.
   - ATL: ATL loads one APK; merge what it needs (native libs from the abi split into the base
     lib/ dir, resources from density/language splits if ATL's resource loader can't read
     splits) into one APK it can run, or teach the ATL launcher to add split paths. Pick the
     working approach, test with a real split app.
   Record in the registry like normal installs (icon from base APK, label).
2. Files app: .xapk/.apks/.apkm show the same install chooser (and an icon/type label for
   bundles), with progress and a clear error (encrypted APKM, missing ABI split, not enough
   space).
3. Verify on the phone: download a real XAPK (e.g. from apkpure/uptodown, a small app) and an
   .apks; install with each runtime; launch; screenshots; uninstall works.
Write ztmetro/files/XAPK.md with results.
