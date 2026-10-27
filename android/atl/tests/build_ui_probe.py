#!/usr/bin/env python3
"""Build a disposable framework probe from an existing demo APK's resources."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import zipfile


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--android-jar", required=True, type=Path)
    parser.add_argument("--base-apk", required=True, type=Path,
                        help="Demo APK declaring com.example.demo_application.MainActivity")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--d8", default="d8")
    args = parser.parse_args()
    sources = Path(__file__).parent / "probes/ui"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ztos-ui-probe-") as directory:
        work = Path(directory)
        classes = work / "classes"
        classes.mkdir()
        run("javac", "-source", "8", "-target", "8", "-cp", args.android_jar,
            "-d", classes, *sorted(sources.glob("*.java")))
        java_archive = work / "classes.jar"
        with zipfile.ZipFile(java_archive, "w") as archive:
            for source in sorted(classes.rglob("*.class")):
                archive.write(source, source.relative_to(classes))
        dex_archive = work / "dex.zip"
        run(args.d8, "--min-api", "23", "--lib", args.android_jar,
            "--output", dex_archive, java_archive)
        unsigned = work / "probe.apk"
        with zipfile.ZipFile(args.base_apk) as base, zipfile.ZipFile(dex_archive) as dex, \
                zipfile.ZipFile(unsigned, "w", zipfile.ZIP_DEFLATED) as output:
            for entry in base.infolist():
                if not entry.filename.startswith("META-INF/") and not (
                        entry.filename.startswith("classes") and entry.filename.endswith(".dex")):
                    output.writestr(entry, base.read(entry))
            for entry in dex.infolist():
                output.writestr(entry, dex.read(entry))
        # This key signs only disposable local tests, never production packages.
        keystore = work / "probe.jks"
        run("keytool", "-genkeypair", "-alias", "probe", "-keystore", keystore,
            "-storepass", "android", "-keypass", "android", "-keyalg", "RSA",
            "-validity", "30", "-dname", "CN=ztOS framework probe", "-noprompt")
        run("jarsigner", "-keystore", keystore, "-storepass", "android",
            "-keypass", "android", "-signedjar", args.output, unsigned, "probe")
    print(args.output)


if __name__ == "__main__":
    main()
