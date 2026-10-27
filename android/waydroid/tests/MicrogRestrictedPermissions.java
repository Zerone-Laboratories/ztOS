import java.lang.reflect.Method;

/** Allow the two restricted permissions required by microG Self-Check, without reinstalling. */
public final class MicrogRestrictedPermissions {
    public static void main(String[] args) throws Exception {
        Class.forName("android.os.Looper").getMethod("prepareMainLooper").invoke(null);
        Class<?> threadClass = Class.forName("android.app.ActivityThread");
        Object thread = threadClass.getMethod("systemMain").invoke(null);
        Object context = threadClass.getMethod("getSystemContext").invoke(thread);
        Object manager = Class.forName("android.content.Context")
                .getMethod("getPackageManager").invoke(context);
        Method allow = Class.forName("android.content.pm.PackageManager").getMethod(
                "addWhitelistedRestrictedPermission", String.class, String.class, int.class);
        for (String permission : new String[] {
                "android.permission.ACCESS_BACKGROUND_LOCATION", "android.permission.RECEIVE_SMS"}) {
            allow.invoke(manager, "com.google.android.gms", permission, 1);
            Object permissions = Class.forName("android.content.pm.PackageManager").getMethod(
                    "getWhitelistedRestrictedPermissions", String.class, int.class)
                    .invoke(manager, "com.google.android.gms", 1);
            if (!(permissions instanceof java.util.Collection)
                    || !((java.util.Collection<?>) permissions).contains(permission)) {
                throw new IllegalStateException("Restricted permission not allowlisted: " + permission);
            }
            System.out.println(permission + " system allowlist verified");
        }
        System.exit(0);
    }
}
