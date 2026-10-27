#!/usr/bin/env python3
"""Build the provider/activity version probe with existing local SDK tools."""
import argparse
from pathlib import Path
import subprocess
import struct
import zipfile


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--android-jar', type=Path, required=True)
    parser.add_argument('--aapt2', type=Path, required=True)
    parser.add_argument('--d8', type=Path, required=True)
    parser.add_argument('--apksig', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    work = args.work.resolve()
    classes = work/'classes'
    classes.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).parent/'oldsdk'
    run('javac', '-source', '8', '-target', '8', '-cp', args.android_jar,
        '-d', classes, *sorted(source.glob('*.java')))
    with zipfile.ZipFile(work/'classes.jar', 'w') as archive:
        for path in classes.rglob('*.class'):
            archive.write(path, path.relative_to(classes))
    run(args.d8, '--min-api', '23', '--lib', args.android_jar,
        '--output', work/'dex.zip', work/'classes.jar')
    run(args.aapt2, 'link', '--manifest', source/'AndroidManifest.xml',
        '-I', args.android_jar, '-o', work/'unsigned.apk')
    with zipfile.ZipFile(work/'unsigned.apk', 'a') as apk, zipfile.ZipFile(work/'dex.zip') as dex:
        entry = zipfile.ZipInfo('classes.dex')
        padding = (-(apk.fp.tell() + 30 + len(entry.filename))) & 3
        if padding:
            entry.extra = struct.pack('<HH', 0xd935, padding) + bytes(padding)
        apk.writestr(entry, dex.read('classes.dex'))
    key = work/'probe.jks'
    if not key.exists():
        run('keytool', '-genkeypair', '-alias', 'probe', '-keystore', key,
            '-storepass', 'android', '-keypass', 'android', '-keyalg', 'RSA',
            '-validity', '365', '-dname', 'CN=ztOS old SDK local probe', '-noprompt')
    signer = work/'SignProbe.java'
    signer.write_text('''import java.io.*;
import java.security.*;
import java.security.cert.X509Certificate;
import java.util.*;
import com.android.apksig.*;
public class SignProbe {
 public static void main(String[] a) throws Exception {
  KeyStore ks = KeyStore.getInstance("PKCS12");
  try (InputStream in = new FileInputStream(a[0])) { ks.load(in, "android".toCharArray()); }
  ApkSigner.SignerConfig config = new ApkSigner.SignerConfig.Builder("probe",
   (PrivateKey)ks.getKey("probe", "android".toCharArray()),
   Collections.singletonList((X509Certificate)ks.getCertificate("probe"))).build();
  new ApkSigner.Builder(Collections.singletonList(config)).setInputApk(new File(a[1]))
   .setOutputApk(new File(a[2])).setMinSdkVersion(23).setV1SigningEnabled(true)
   .setV2SigningEnabled(true).setV3SigningEnabled(false).build().sign();
  ApkVerifier.Result result = new ApkVerifier.Builder(new File(a[2])).build().verify();
  if (!result.isVerified()) throw new IllegalStateException(result.getErrors().toString());
  System.out.println("PASS: probe APK signature verified; v2=" + result.isVerifiedUsingV2Scheme());
 }
}
''')
    run('javac', '-cp', args.apksig, '-d', work, signer)
    run('java', '-cp', str(work) + ':' + str(args.apksig.resolve()), 'SignProbe',
        key, work/'unsigned.apk', work/'oldsdk-probe.apk')


if __name__ == '__main__':
    main()
