import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.AlgorithmParameterSpec;
import java.security.cert.Certificate;
import java.security.cert.X509Certificate;

/** Requests an EC key with challenge and Android device-property attestation. */
public final class KeyMintAttestationProbe {
    private static final String ALIAS = "ztos-waydroid-id-attestation-probe";

    public static void main(String[] args) throws Exception {
        Class<?> providerClass = Class.forName(
                "android.security.keystore2.AndroidKeyStoreProvider");
        providerClass.getMethod("install").invoke(null);
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        store.deleteEntry(ALIAS);
        try {
            Class<?> builderClass = Class.forName(
                    "android.security.keystore.KeyGenParameterSpec$Builder");
            Constructor<?> constructor = builderClass.getConstructor(String.class, int.class);
            Object builder = constructor.newInstance(ALIAS, 4 | 8); // SIGN | VERIFY
            invoke(builderClass, builder, "setAlgorithmParameterSpec",
                    AlgorithmParameterSpec.class, new ECGenParameterSpec("secp256r1"));
            invoke(builderClass, builder, "setDigests", String[].class,
                    new String[] {"SHA-256"});
            invoke(builderClass, builder, "setAttestationChallenge", byte[].class,
                    "ztos-waydroid-id-attestation".getBytes(StandardCharsets.UTF_8));
            invoke(builderClass, builder, "setDevicePropertiesAttestationIncluded",
                    boolean.class, true);
            AlgorithmParameterSpec spec = (AlgorithmParameterSpec) builderClass
                    .getMethod("build").invoke(builder);
            boolean includeDeviceProperties = (boolean) spec.getClass()
                    .getMethod("isDevicePropertiesAttestationIncluded").invoke(spec);
            System.out.println("devicePropertiesAttestationIncluded=" + includeDeviceProperties);

            KeyPairGenerator generator = KeyPairGenerator.getInstance("EC", "AndroidKeyStore");
            generator.initialize(spec);
            KeyPair pair = generator.generateKeyPair();
            Certificate[] chain = store.getCertificateChain(ALIAS);
            if (chain == null || chain.length == 0)
                throw new IllegalStateException("attestation certificate chain is empty");
            boolean hasAttestationExtension = ((X509Certificate) chain[0])
                    .getExtensionValue("1.3.6.1.4.1.11129.2.1.17") != null;
            System.out.println("PASS: EC key generated; attestation chain length=" + chain.length
                    + "; attestation extension=" + hasAttestationExtension
                    + "; public key=" + pair.getPublic().getAlgorithm());
        } finally {
            store.deleteEntry(ALIAS);
        }
        System.exit(0); // app_process otherwise stays alive on Binder worker threads.
    }

    private static void invoke(Class<?> type, Object target, String name,
            Class<?> parameterType, Object value) throws Exception {
        Method method = type.getMethod(name, parameterType);
        method.invoke(target, value);
    }
}
