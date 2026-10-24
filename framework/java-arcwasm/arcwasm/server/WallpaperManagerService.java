// arc-wasm: the "wallpaper" service, a lean IWallpaperManager (topic
// userspace, docs/topics/userspace.md). There is no wallpaper file and no
// live wallpaper: getWallpaper answers null, so WallpaperManager falls back
// to the system default (framework-res default_wallpaper), as on a fresh
// device. The launcher's size hints are kept and answered back. Setting a
// wallpaper throws (arcwasm.server.base.WallpaperManagerBase).
package arcwasm.server;

import android.app.IWallpaperManagerCallback;
import android.app.WallpaperInfo;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;

import arcwasm.server.base.WallpaperManagerBase;

public final class WallpaperManagerService extends WallpaperManagerBase {
    private int mWidth = -1;
    private int mHeight = -1;

    @Override
    public ParcelFileDescriptor getWallpaper(IWallpaperManagerCallback cb, Bundle outParams) {
        if (outParams != null) {
            outParams.putInt("width", mWidth);
            outParams.putInt("height", mHeight);
        }
        return null;
    }

    @Override public WallpaperInfo getWallpaperInfo() { return null; }
    @Override public boolean hasNamedWallpaper(String name) { return false; }

    @Override
    public synchronized void setDimensionHints(int width, int height) {
        mWidth = width;
        mHeight = height;
    }

    @Override public synchronized int getWidthHint() { return mWidth; }
    @Override public synchronized int getHeightHint() { return mHeight; }
}
