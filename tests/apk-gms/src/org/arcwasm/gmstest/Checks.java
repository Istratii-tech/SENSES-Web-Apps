package org.arcwasm.gmstest;

import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

import com.android.billingclient.api.BillingClient;
import com.android.billingclient.api.BillingClientStateListener;
import com.android.billingclient.api.BillingResult;
import com.android.billingclient.api.PendingPurchasesParams;
import com.android.billingclient.api.Purchase;
import com.android.billingclient.api.PurchasesUpdatedListener;
import com.google.android.gms.ads.identifier.AdvertisingIdClient;
import com.google.android.gms.auth.api.signin.GoogleSignIn;
import com.google.android.gms.auth.api.signin.GoogleSignInOptions;
import com.google.android.gms.cloudmessaging.Rpc;
import com.google.android.gms.common.GoogleApiAvailability;
import com.google.android.gms.common.api.ApiException;
import com.google.android.gms.location.LocationServices;
import com.google.android.gms.maps.MapsInitializer;
import com.google.android.gms.safetynet.SafetyNet;
import com.google.android.gms.tasks.Task;
import com.google.android.gms.tasks.Tasks;
import com.google.android.play.core.appupdate.AppUpdateInfo;
import com.google.android.play.core.appupdate.AppUpdateManagerFactory;
import com.google.android.play.core.assetpacks.AssetPackManagerFactory;
import com.google.android.play.core.integrity.IntegrityManagerFactory;
import com.google.android.play.core.integrity.IntegrityTokenRequest;
import com.google.android.play.core.review.ReviewManagerFactory;
import com.google.firebase.FirebaseApp;
import com.google.firebase.FirebaseOptions;
import com.google.firebase.analytics.FirebaseAnalytics;
import com.google.firebase.messaging.FirebaseMessaging;

import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/**
 * What apps do with Google Play services, through the real client
 * libraries; each check returns one line saying what came back (GmsTest
 * compares it with what a phone with Google Play services and no account
 * gives, or, with ro.arcwasm.gms=0, a phone without them). Called on a
 * thread of the test's, not the main thread (Tasks.await).
 */
public final class Checks {
    private static final long TIMEOUT_S = 40;

    private Checks() {}

    /** A Task's outcome: "ok RESULT", "api CODE" (ApiException), "error CLASS: MESSAGE" or "timeout". */
    static String await(Task<?> task) {
        try {
            Object r = Tasks.await(task, TIMEOUT_S, TimeUnit.SECONDS);
            return "ok " + r;
        } catch (ExecutionException e) {
            Throwable c = e.getCause();
            if (c instanceof ApiException) {
                return "api " + ((ApiException) c).getStatusCode();
            }
            return "error " + c.getClass().getName() + ": " + c.getMessage();
        } catch (TimeoutException e) {
            return "timeout";
        } catch (InterruptedException e) {
            return "interrupted";
        }
    }

    /** Runs |r| on the main thread and waits. */
    static void onMain(final Runnable r) throws InterruptedException {
        final CountDownLatch done = new CountDownLatch(1);
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                try {
                    r.run();
                } finally {
                    done.countDown();
                }
            }
        });
        done.await(TIMEOUT_S, TimeUnit.SECONDS);
    }

    public static String availability(Context c) throws Exception {
        int r = GoogleApiAvailability.getInstance().isGooglePlayServicesAvailable(c);
        String version;
        try {
            version = "" + c.getPackageManager().getPackageInfo("com.google.android.gms", 0).versionCode;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            version = "none";
        }
        return "status " + r + " version " + version;
    }

    public static String advertisingId(Context c) {
        try {
            AdvertisingIdClient.Info info = AdvertisingIdClient.getAdvertisingIdInfo(c);
            return "id " + info.getId() + " limited " + info.isLimitAdTrackingEnabled();
        } catch (Exception e) {
            return "error " + e.getClass().getName() + ": " + e.getMessage();
        }
    }

    /**
     * Instance ID's registration through play-services-cloud-messaging's Rpc
     * (MessengerIpcClient to GmsCore's c2dm REGISTER service), twice, then
     * its deletion: "token length N", "error ERROR" (the reply's), or what
     * the Task gave.
     */
    public static String rpcToken(Context c) {
        Bundle b = new Bundle();
        b.putString("scope", "*");
        b.putString("sender", "123456789012");
        b.putString("subtype", "123456789012");
        b.putString("appid", "cArcWasmTestFid0000000");
        Rpc rpc = new Rpc(c);
        String[] first = reply(rpc.send(b));
        String[] second = reply(rpc.send(new Bundle(b)));
        Bundle d = new Bundle(b);
        d.putString("delete", "1");
        String[] delete = reply(rpc.send(d));
        return first[0] + " | same " + (first[1] != null && first[1].equals(second[1]))
                + " | delete " + delete[0];
    }

    /** A registration reply: {description, token or null}. */
    static String[] reply(Task<Bundle> task) {
        String r = await(task);
        if (!r.startsWith("ok ")) {
            return new String[] {r, null};
        }
        Bundle b = task.getResult();
        if (b == null) {
            return new String[] {"null", null};
        }
        String token = b.getString("registration_id");
        if (token != null) {
            return new String[] {"token length " + token.length(), token};
        }
        if (b.containsKey("unregistered")) {
            return new String[] {"unregistered", null};
        }
        if (b.containsKey("error")) {
            return new String[] {"error " + b.getString("error"), null};
        }
        return new String[] {"keys " + new java.util.TreeSet<String>(b.keySet()), null};
    }

    /** Firebase initialized by hand (no google-services.json), Analytics, then FCM's token. */
    public static String firebase(final Context c) throws Exception {
        final FirebaseOptions options = new FirebaseOptions.Builder()
                .setApplicationId("1:123456789012:android:0123456789abcdef")
                .setApiKey("AIzaSyArcWasmTestKeyNotARealKey0000000")
                .setProjectId("arcwasm-gms-test")
                .setGcmSenderId("123456789012")
                .build();
        final FirebaseApp[] app = new FirebaseApp[1];
        onMain(new Runnable() {
            @Override
            public void run() {
                app[0] = FirebaseApp.initializeApp(c, options);
            }
        });
        if (app[0] == null) {
            return "no FirebaseApp";
        }
        FirebaseAnalytics.getInstance(c).logEvent("arcwasm_gms_test", new Bundle());
        String token = await(FirebaseMessaging.getInstance().getToken());
        return "app " + app[0].getName() + " analytics logged; token "
                + (token.startsWith("ok ") ? "ok length " + (token.length() - 3) : token);
    }

    public static String signIn(Context c) {
        Object last = GoogleSignIn.getLastSignedInAccount(c);
        String silent = await(GoogleSignIn.getClient(c, GoogleSignInOptions.DEFAULT_SIGN_IN)
                .silentSignIn());
        return "last " + last + " silent " + silent;
    }

    public static String location(Context c) {
        return await(LocationServices.getFusedLocationProviderClient(c).getLastLocation());
    }

    public static String safetyNet(Context c) {
        byte[] nonce = "arc-wasm-gms-test-nonce".getBytes();
        return await(SafetyNet.getClient(c).attest(nonce, "AIzaSyArcWasmTestKeyNotARealKey0000000"));
    }

    public static String billing(final Context c) throws Exception {
        final int[] code = {-100};
        final CountDownLatch done = new CountDownLatch(1);
        final BillingClient[] client = new BillingClient[1];
        onMain(new Runnable() {
          @Override
          public void run() {
            client[0] = BillingClient.newBuilder(c)
                    .setListener(new PurchasesUpdatedListener() {
                        @Override
                        public void onPurchasesUpdated(BillingResult result, List<Purchase> purchases) {
                        }
                    })
                    .enablePendingPurchases(PendingPurchasesParams.newBuilder()
                            .enableOneTimeProducts().build())
                    .build();
            client[0].startConnection(new BillingClientStateListener() {
                @Override
                public void onBillingSetupFinished(BillingResult result) {
                    code[0] = result.getResponseCode();
                    done.countDown();
                }

                @Override
                public void onBillingServiceDisconnected() {
                    code[0] = -101;
                    done.countDown();
                }
            });
          }
        });
        if (!done.await(TIMEOUT_S, TimeUnit.SECONDS)) {
            return "timeout";
        }
        return "setup " + code[0];
    }

    public static String appUpdate(Context c) {
        Task<AppUpdateInfo> t = AppUpdateManagerFactory.create(c).getAppUpdateInfo();
        String r = await(t);
        if (r.startsWith("ok ")) {
            AppUpdateInfo info = t.getResult();
            return "availability " + info.updateAvailability() + " status " + info.installStatus();
        }
        return r;
    }

    public static String review(Context c) {
        return await(ReviewManagerFactory.create(c).requestReviewFlow());
    }

    public static String integrity(Context c) {
        return await(IntegrityManagerFactory.create(c).requestIntegrityToken(
                IntegrityTokenRequest.builder().setNonce("YXJjLXdhc20tZ21zLXRlc3Qtbm9uY2U")
                        .setCloudProjectNumber(123456789012L).build()));
    }

    public static String maps(final Context c) throws Exception {
        final String[] r = new String[1];
        onMain(new Runnable() {
            @Override
            public void run() {
                try {
                    r[0] = "initialize " + MapsInitializer.initialize(c);
                } catch (Throwable t) {
                    android.util.Log.w("GmsTest", "maps", t);
                    r[0] = "error " + t;
                }
            }
        });
        return r[0];
    }

    public static String assetPacks(Context c) {
        return await(AssetPackManagerFactory.getInstance(c)
                .getPackStates(Collections.singletonList("arcwasm_pack")));
    }
}
