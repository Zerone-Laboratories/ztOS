#!/system/bin/sh
# app_process needs the same runtime paths and boot classpath as Android init.
for source in /init.environ.rc /data/system/environ/classpath; do
    while read -r kind key value; do
        if [ "$kind" = export ]; then
            export "$key=$value"
        fi
    done < "$source"
done
export CLASSPATH=/data/local/tmp/audio-robust.dex
exec app_process /system/bin AudioRobustTest "${1:-12}"
