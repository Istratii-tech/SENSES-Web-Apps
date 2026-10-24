// arc-wasm: the "location" service, a lean ILocationManager
// (docs/topics/activity.md). There are no location providers yet (a browser
// one over navigator.geolocation would be the natural first): apps see a
// device whose providers are all off, which they handle, instead of a
// LocationManager with no service behind it (NullPointerException).
package arcwasm.server;

import android.app.PendingIntent;
import android.location.Criteria;
import android.location.Geofence;
import android.location.IGpsStatusListener;
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
    public boolean providerMeetsCriteria(String provider, Criteria criteria) {
        return false;
    }

    @Override
    public boolean isProviderEnabled(String provider) {
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
    public boolean addGpsStatusListener(IGpsStatusListener listener, String packageName) {
        return false;
    }

    @Override
    public void removeGpsStatusListener(IGpsStatusListener listener) {
    }

    @Override
    public boolean geocoderIsPresent() {
        return false;
    }

    @Override
    public void locationCallbackFinished(ILocationListener listener) {
    }
}
