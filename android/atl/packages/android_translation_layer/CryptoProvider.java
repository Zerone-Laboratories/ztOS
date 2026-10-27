package android.security;

import java.security.Provider;

/** Restores BC algorithms and the identifiers used by certificate validators. */
public final class CryptoProvider extends Provider {
    public CryptoProvider() {
        super("ztOSCrypto", 1.1, "ztOS ART signature mappings backed by Bouncy Castle");
        add("KeyFactory", "EC", "com.android.org.bouncycastle.jcajce.provider.asymmetric.ec.KeyFactorySpi$EC");
        add("KeyPairGenerator", "EC", "com.android.org.bouncycastle.jcajce.provider.asymmetric.ec.KeyPairGeneratorSpi$EC");
        add("KeyFactory", "RSA", "com.android.org.bouncycastle.jcajce.provider.asymmetric.rsa.KeyFactorySpi");
        add("KeyPairGenerator", "RSA", "com.android.org.bouncycastle.jcajce.provider.asymmetric.rsa.KeyPairGeneratorSpi");
        String[] hashes = {"1", "224", "256", "384", "512"};
        String[] rsaOids = {"1.2.840.113549.1.1.5", "1.2.840.113549.1.1.14",
                "1.2.840.113549.1.1.11", "1.2.840.113549.1.1.12", "1.2.840.113549.1.1.13"};
        String[] ecOids = {"1.2.840.10045.4.1", "1.2.840.10045.4.3.1",
                "1.2.840.10045.4.3.2", "1.2.840.10045.4.3.3", "1.2.840.10045.4.3.4"};
        for (int i = 0; i < hashes.length; i++) {
            String hash = hashes[i];
            signature("SHA" + hash + "withECDSA", ecOids[i],
                    "com.android.org.bouncycastle.jcajce.provider.asymmetric.ec.SignatureSpi$ecDSA"
                    + (hash.equals("1") ? "" : hash));
            signature("SHA" + hash + "withRSA", rsaOids[i],
                    "com.android.org.bouncycastle.jcajce.provider.asymmetric.rsa.DigestSignatureSpi$SHA" + hash);
        }
        alias("KeyFactory", "1.2.840.113549.1.1.1", "RSA");
        alias("KeyFactory", "1.2.840.10045.2.1", "EC");
    }

    private void signature(String name, String oid, String implementation) {
        if (add("Signature", name, implementation)) alias("Signature", oid, name);
    }

    private void alias(String type, String oid, String name) {
        put("Alg.Alias." + type + "." + oid, name);
        put("Alg.Alias." + type + ".OID." + oid, name);
    }

    private boolean add(String type, String name, String implementation) {
        try {
            Class.forName(implementation);
            put(type + "." + name, implementation);
            return true;
        } catch (ClassNotFoundException ignored) {
            return false;
        }
    }
}
