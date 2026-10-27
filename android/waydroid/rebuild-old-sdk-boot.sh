#!/system/bin/sh
# Run as Android root after deploying the JARs. Build the framework boot extension.
set -eu
out=/data/local/tmp/ztos-oldsdk-boot
mkdir -p "$out"
sha256sum /system/framework/framework.jar /system/framework/telephony-common.jar \
    > "$out/framework-jars.sha256"
classpath=/apex/com.android.art/javalib/core-oj.jar:/apex/com.android.art/javalib/core-libart.jar:/apex/com.android.art/javalib/okhttp.jar:/apex/com.android.art/javalib/bouncycastle.jar:/apex/com.android.art/javalib/apache-xml.jar
extension=/system/framework/framework.jar:/system/framework/framework-graphics.jar:/system/framework/ext.jar:/system/framework/telephony-common.jar:/system/framework/voip-common.jar:/system/framework/ims-common.jar:/apex/com.android.i18n/javalib/core-icu4j.jar
classpath=$classpath:$extension
set --
oldifs=$IFS
IFS=:
for jar in $extension; do
    set -- "$@" "--dex-file=$jar" "--dex-location=$jar"
done
IFS=$oldifs
/apex/com.android.art/bin/dex2oat64 \
    --runtime-arg "-Xbootclasspath:$classpath" \
    --runtime-arg "-Xbootclasspath-locations:$classpath" \
    --boot-image=/system/framework/boot.art \
    --image="$out/boot.art" --oat-file="$out/boot.oat" \
    --output-vdex="$out/boot-framework.vdex" \
    --oat-location=/system/framework/arm64/boot.oat \
    --single-image --image-format=uncompressed --compiler-filter=speed-profile \
    --profile-file=/system/etc/boot-image.prof --instruction-set=arm64 \
    --instruction-set-features=runtime --runtime-arg -Xms64m --runtime-arg -Xmx256m \
    --generate-mini-debug-info "$@"
chmod 644 "$out"/*
