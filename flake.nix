{
  description = "ztOS development shell for the Arch Linux ARM port";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # The installer boots the Arch Linux ARM disk through aarch64 UEFI. On an
      # ARM host that is the native firmware; on x86-64 QEMU needs the
      # aarch64 build of the same firmware.
      aarch64Firmware =
        pkgs:
        if pkgs.stdenv.hostPlatform.isAarch64 then pkgs.OVMF else pkgs.pkgsCross.aarch64-multiplatform.OVMF;

      mkDevShell =
        pkgs:
        let
          ovmf = aarch64Firmware pkgs;
        in
        pkgs.mkShell {
          packages = with pkgs; [
            # C toolchain and the ztMETRO build
            bashInteractive
            coreutils
            findutils
            diffutils
            gnused
            gnugrep
            gawk
            gnutar
            gzip
            bzip2
            patch
            pkg-config
            gnumake
            cmake
            ninja
            meson
            gcc
            zlib
            glib
            wayland-scanner
            wayland
            wayland-protocols
            wlroots_0_20
            libxkbcommon
            cairo
            pango
            librsvg
            gdk-pixbuf
            pixman
            libdrm
            libGL
            mesa
            libinput
            libpulseaudio
            polkit
            systemd

            # Arch Linux ARM image and disk tooling
            python3
            fakeroot
            libarchive
            util-linux
            dosfstools
            mtools
            e2fsprogs
            gptfdisk
            parted
            cpio
            xz
            zstd
            unzip
            zip

            # Kernel and boot image work
            bc
            bison
            flex
            elfutils
            openssl
            ncurses
            perl
            pahole
            kmod
            dtc
            rsync
            file
            which

            # Networking, devices and screenshots
            git
            curl
            wget
            openssh
            socat
            android-tools
            imagemagick

            # The emulator: system and user mode QEMU, its tools, and the
            # aarch64 UEFI firmware that the installer passes with -bios.
            qemu
            qemu-utils
            qemu-user
            ovmf
          ];

          shellHook = ''
            export ZTOS_EDK2="${ovmf.fd}/FV"

            echo "ztOS development shell (Arch Linux ARM port)"
            echo "  installer: ./installer/ztos --help"
            echo "  emulator:  ./installer/ztos emulator"
            echo "  firmware:  $ZTOS_EDK2"
          '';
        };
    in
    {
      devShells = forAllSystems (pkgs: {
        default = mkDevShell pkgs;
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
