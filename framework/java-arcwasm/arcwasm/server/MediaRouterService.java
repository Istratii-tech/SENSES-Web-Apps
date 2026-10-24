// arc-wasm: the "media_router" service, a lean IMediaRouterService (topic
// userspace, docs/topics/userspace.md). MediaRouter (SystemUI's quick
// settings ask for it) registers a client here; there are no remote displays
// or media route providers, so every client's state has no routes and
// discovery, selection and volume requests change nothing.
package arcwasm.server;

import android.media.IMediaRouterClient;
import android.media.MediaRouterClientState;

import arcwasm.server.base.MediaRouterBase;

public final class MediaRouterService extends MediaRouterBase {
    @Override public void registerClientAsUser(IMediaRouterClient client, String pkg, int userId) {}
    @Override public void unregisterClient(IMediaRouterClient client) {}
    @Override public MediaRouterClientState getState(IMediaRouterClient client) {
        return new MediaRouterClientState();
    }
    @Override public void setDiscoveryRequest(IMediaRouterClient client, int routeTypes,
            boolean activeScan) {}
    @Override public void setSelectedRoute(IMediaRouterClient client, String routeId,
            boolean explicit) {}
    @Override public void requestSetVolume(IMediaRouterClient client, String routeId, int volume) {}
    @Override public void requestUpdateVolume(IMediaRouterClient client, String routeId,
            int direction) {}
}
