// arc-wasm: the "location" service on Android 6.0.1, a lean ILocationManager
// (as the 4.4 build's, docs/topics/activity.md). There are no location providers yet (a browser
// one over navigator.geolocation would be the natural first): apps see a
// device whose providers are all off, which they handle, instead of a
// LocationManager with no service behind it (NullPointerException).
package arcwasm.server;

import android.app.PendingIntent;
import android.location.Criteria;
import android.location.Geofence;
import android.location.IGnssStatusListener;
import android.location.ILocationListener;
import android.location.Location;
import android.location.LocationRequest;

import arcwasm.server.base.LocationManagerBase;

import java.util.ArrayList;
import java.util.List;

public final class LocationManagerService extends LocationManagerBase {
    @Override
    public List<String> getAllProviders() {
        return new ArrayList<String>();
    }

    @Override
    public List<String> getProviders(Criteria criteria, boolean enabledOnly) {
        return new ArrayList<String>();
    }

    @Override
    public String getBestProvider(Criteria criteria, boolean enabledOnly) {
        return null;
    }

    @Override
    public boolean isProviderEnabledForUser(String provider, int userId) {
        return false;
    }

    /** API 28's LocationManager.isLocationEnabled: location is off. */
    @Override
    public boolean isLocationEnabledForUser(int userId) {
        return false;
    }

    @Override
    public void requestLocationUpdates(LocationRequest request, ILocationListener listener,
            PendingIntent intent, String packageName) {
        // No provider will ever report; nothing to register.
    }

    @Override
    public void removeUpdates(ILocationListener listener, PendingIntent intent,
            String packageName) {
    }

    @Override
    public void requestGeofence(LocationRequest request, Geofence geofence,
            PendingIntent intent, String packageName) {
    }

    @Override
    public void removeGeofence(Geofence fence, PendingIntent intent, String packageName) {
    }

    @Override
    public Location getLastLocation(LocationRequest request, String packageName) {
        return null;
    }

    @Override
    public boolean registerGnssStatusCallback(IGnssStatusListener listener, String packageName) {
        return false;
    }

    @Override
    public void unregisterGnssStatusCallback(IGnssStatusListener listener) {
    }

    @Override
    public String[] getBackgroundThrottlingWhitelist() {
        return new String[0];
    }

    @Override
    public int getGnssYearOfHardware() {
        return 0;
    }

    @Override
    public boolean geocoderIsPresent() {
        return false;
    }

    @Override
    public void locationCallbackFinished(ILocationListener listener) {
    }
}
