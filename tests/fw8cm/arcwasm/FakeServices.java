package arcwasm;

import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.ServiceManager;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Proxy;
import java.util.Map;

/**
 * Minimal "audio" and "appops" services for the tests of topic
 * fw8-crypto-media when the real (light) ones of topic fw8-system are not
 * running: AudioTrack and MediaPlayer 8.1 (PlayerBase) report every player
 * to IAudioService and ask IAppOpsService for OP_PLAY_AUDIO, and are muted
 * (or throw) without them. Each fake is a dynamic proxy of the interface
 * behind a local Binder, put into ServiceManager's cache: every call returns
 * the type's zero (MODE_ALLOWED for app ops), trackPlayer a new player id.
 */
final class FakeServices {
    private FakeServices() {}

    static void installIfMissing() throws Exception {
        install("appops", "com.android.internal.app.IAppOpsService");
        install("audio", "android.media.IAudioService");
    }

    private static void install(String name, String iface) throws Exception {
        if (ServiceManager.getService(name) != null) return;
        final Class<?> c = Class.forName(iface);
        final Binder binder = new Binder();
        final int[] ids = {0};
        InvocationHandler h = (proxy, m, args) -> {
            String n = m.getName();
            if (n.equals("asBinder")) return binder;
            if (n.equals("trackPlayer")) return ++ids[0];
            if (n.equals("toString")) return "fake " + name + " service";
            if (n.equals("hashCode")) return System.identityHashCode(proxy);
            if (n.equals("equals")) return proxy == args[0];
            Class<?> r = m.getReturnType();
            if (r == boolean.class) return false;
            if (r == int.class) return 0;
            if (r == long.class) return 0L;
            if (r == float.class) return 0f;
            if (r == void.class) return null;
            if (r.isPrimitive()) return (byte) 0;
            return null;
        };
        IInterface p = (IInterface) Proxy.newProxyInstance(c.getClassLoader(), new Class<?>[] {c}, h);
        Field d = Class.forName(iface + "$Stub").getDeclaredField("DESCRIPTOR");
        d.setAccessible(true);
        binder.attachInterface(p, (String) d.get(null));
        Field f = ServiceManager.class.getDeclaredField("sCache");
        f.setAccessible(true);
        @SuppressWarnings("unchecked")
        Map<String, IBinder> cache = (Map<String, IBinder>) f.get(null);
        cache.put(name, binder);
        System.out.println("       (no \"" + name + "\" service: a fake one for this test)");
    }
}
