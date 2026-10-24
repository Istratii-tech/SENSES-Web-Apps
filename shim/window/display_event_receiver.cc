// libgui's DisplayEventReceiver (frameworks/native/include/gui/
// DisplayEventReceiver.h) for the in-process SurfaceFlinger: the same class
// the real android_view_DisplayEventReceiver.cpp (Choreographer's vsync)
// uses; only the connection comes from arcwasm::SurfaceFlinger instead of
// ComposerService (ISurfaceComposer). The rest is
// frameworks/native/libs/gui/DisplayEventReceiver.cpp as it is, over the
// real BitTube and IDisplayEventConnection. Part of the arc-wasm shim layer
// (topic "window", docs/topics/window.md).

#include <gui/BitTube.h>
#include <gui/DisplayEventReceiver.h>
#include <gui/IDisplayEventConnection.h>

#include "surface_flinger.h"

namespace android {

DisplayEventReceiver::DisplayEventReceiver() {
  mEventConnection = arcwasm::SurfaceFlinger::Get()->CreateDisplayEventConnection();
  if (mEventConnection != NULL)
    mDataChannel = mEventConnection->getDataChannel();
}

DisplayEventReceiver::~DisplayEventReceiver() {}

status_t DisplayEventReceiver::initCheck() const {
  return mDataChannel != NULL ? NO_ERROR : NO_INIT;
}

int DisplayEventReceiver::getFd() const {
  return mDataChannel != NULL ? mDataChannel->getFd() : NO_INIT;
}

status_t DisplayEventReceiver::setVsyncRate(uint32_t count) {
  if (int32_t(count) < 0)
    return BAD_VALUE;
  if (mEventConnection == NULL)
    return NO_INIT;
  mEventConnection->setVsyncRate(count);
  return NO_ERROR;
}

status_t DisplayEventReceiver::requestNextVsync() {
  if (mEventConnection == NULL)
    return NO_INIT;
  mEventConnection->requestNextVsync();
  return NO_ERROR;
}

ssize_t DisplayEventReceiver::getEvents(DisplayEventReceiver::Event* events, size_t count) {
  return DisplayEventReceiver::getEvents(mDataChannel, events, count);
}

ssize_t DisplayEventReceiver::getEvents(const sp<BitTube>& dataChannel, Event* events,
                                        size_t count) {
  return BitTube::recvObjects(dataChannel, events, count);
}

ssize_t DisplayEventReceiver::sendEvents(const sp<BitTube>& dataChannel, Event const* events,
                                         size_t count) {
  return BitTube::sendObjects(dataChannel, events, count);
}

}  // namespace android
