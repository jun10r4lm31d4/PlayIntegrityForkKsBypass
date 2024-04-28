package es.chiteroman.playintegrityfix;

import java.security.Provider;

public final class CustomProvider extends Provider {

    CustomProvider(Provider provider) {
        super(provider.getName(), provider.getVersion(), provider.getInfo());
        EntryPoint.LOG("Loading new provider");
        putAll(provider);
        this.put("KeyStore.AndroidKeyStore", CustomKeyStoreSpi.class.getName());
        this.put("KeyPairGenerator.EC", CustomKeyStoreKeyPairGeneratorSpi.EC.class.getName());
        this.put("KeyPairGenerator.RSA", CustomKeyStoreKeyPairGeneratorSpi.RSA.class.getName());
        this.put("KeyPairGenerator.OLDEC", provider.get("KeyPairGenerator.EC"));
        this.put("KeyPairGenerator.OLDRSA", provider.get("KeyPairGenerator.RSA"));
    }

    @Override
    public synchronized Service getService(String type, String algorithm) {
        if (EntryPoint.getVerboseLogs() > 2) EntryPoint.LOG(String.format("Service: Caller type '%s' with algorithm '%s'", type, algorithm));
        if (type.equals("KeyStore")) EntryPoint.spoofDevice();
        return super.getService(type, algorithm);
    }
}
