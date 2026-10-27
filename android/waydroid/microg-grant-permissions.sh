#!/system/bin/sh
# Run inside Android as root after privileged microG installation.
set -eu
# Root can grant restricted runtime permissions only after system allowlisting.
CLASSPATH=/data/local/tmp/MicrogRestrictedPermissions.dex \
    app_process /system/bin MicrogRestrictedPermissions
for permission in ACCESS_COARSE_LOCATION ACCESS_FINE_LOCATION ACCESS_BACKGROUND_LOCATION \
    GET_ACCOUNTS READ_PHONE_STATE POST_NOTIFICATIONS READ_CONTACTS \
    READ_EXTERNAL_STORAGE WRITE_EXTERNAL_STORAGE RECEIVE_SMS; do
    pm grant com.google.android.gms "android.permission.$permission"
done
appops set com.google.android.gms SYSTEM_ALERT_WINDOW allow
cmd deviceidle whitelist +com.google.android.gms
cmd deviceidle whitelist +com.google.android.gsf
cmd deviceidle whitelist +com.android.vending
