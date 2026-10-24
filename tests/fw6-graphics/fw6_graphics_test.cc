// Native tests of topic fw6-graphics (Android 6.0.1 graphics in
// WebAssembly): Skia (branch m), hwui's software Canvas (SkiaCanvas),
// minikin text layout with Roboto, libgui 6.0 (SurfaceComposerClient,
// Surface, BufferQueue) over the in-process SurfaceFlinger and the software
// gralloc, vsync through DisplayEventReceiver, screenshots, and libinput's
// InputChannel. Everything draws into memory and checks pixels.
//
//   make fw6-graphics-test   (build/fw6g/fw6_graphics_test.js under Node)

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include <SkBitmap.h>
#include <SkCanvas.h>
#include <SkPaint.h>
#include <SkPath.h>
#include <SkTypeface.h>
#include <SkFontMgr.h>
#include <SkImageDecoder.h>
#include <SkImageEncoder.h>
#include <SkStream.h>
#include <SkData.h>
#include <SkForceLinking.h>
#include <android/native_window.h>

#include <Canvas.h>  // hwui
#include <RenderNode.h>
#include <DisplayListCanvas.h>

#include <minikin/FontFamily.h>
#include <minikin/Layout.h>

#include "MinikinSkia.h"
#include "MinikinUtils.h"
#include "Paint.h"
#include "TypefaceImpl.h"

#include <gui/DisplayEventReceiver.h>
#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SurfaceControl.h>
#include <input/Input.h>
#include <input/InputTransport.h>
#include <ui/DisplayInfo.h>
#include <ui/Rect.h>
#include <utils/Timers.h>

#include "surface_flinger6.h"

extern "C" int arc_art_boot_kernel();

using namespace android;

namespace {

SkColor PixelAt(const SkBitmap& bitmap, int x, int y) {
  return bitmap.getColor(x, y);
}

TEST(Core, String8) {
  String8 a;
  EXPECT_STREQ("", a.string());
  a.setTo("abc");
  EXPECT_STREQ("abc", a.string());
  String8 b("x.y");
  b.setTo("z");
  EXPECT_STREQ("z", b.string());
}

// --- Skia -----------------------------------------------------------------------------------

TEST(Skia, RectCircleAndPath) {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(64, 64);
  bitmap.eraseColor(SK_ColorWHITE);
  SkCanvas canvas(bitmap);
  SkPaint paint;
  paint.setColor(SK_ColorRED);
  canvas.drawRect(SkRect::MakeLTRB(4, 4, 20, 20), paint);
  EXPECT_EQ(SK_ColorRED, PixelAt(bitmap, 4, 4));
  EXPECT_EQ(SK_ColorRED, PixelAt(bitmap, 19, 19));
  EXPECT_EQ(SK_ColorWHITE, PixelAt(bitmap, 20, 20));
  paint.setColor(SK_ColorBLUE);
  paint.setAntiAlias(true);
  canvas.drawCircle(40, 40, 10, paint);
  EXPECT_EQ(SK_ColorBLUE, PixelAt(bitmap, 40, 40));
  EXPECT_EQ(SK_ColorWHITE, PixelAt(bitmap, 40, 52));
  const SkColor edge = PixelAt(bitmap, 50, 40);  // on the edge: blended
  EXPECT_NE(SK_ColorBLUE, edge);
  SkPath triangle;
  triangle.moveTo(0, 63);
  triangle.lineTo(30, 63);
  triangle.lineTo(0, 33);
  triangle.close();
  paint.setColor(SK_ColorGREEN);
  paint.setAntiAlias(false);
  canvas.drawPath(triangle, paint);
  EXPECT_EQ(SK_ColorGREEN, PixelAt(bitmap, 2, 61));
  EXPECT_EQ(SK_ColorWHITE, PixelAt(bitmap, 28, 36));
}

TEST(Skia, PngRoundTrip) {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(8, 8);
  bitmap.eraseColor(SkColorSetARGB(255, 10, 200, 30));
  *bitmap.getAddr32(3, 3) = SkPreMultiplyColor(SkColorSetARGB(128, 255, 0, 0));
  SkAutoTDelete<SkImageEncoder> encoder(SkImageEncoder::Create(SkImageEncoder::kPNG_Type));
  ASSERT_TRUE(encoder.get() != nullptr) << "the PNG encoder registers itself";
  SkDynamicMemoryWStream out;
  ASSERT_TRUE(SkImageEncoder::EncodeStream(&out, bitmap, SkImageEncoder::kPNG_Type, 100));
  SkAutoTUnref<SkData> data(out.copyToData());
  EXPECT_GT(data->size(), 20u);
  SkBitmap decoded;
  ASSERT_TRUE(SkImageDecoder::DecodeMemory(data->data(), data->size(), &decoded));
  EXPECT_EQ(8, decoded.width());
  EXPECT_EQ(SkColorSetARGB(255, 10, 200, 30), decoded.getColor(0, 0));
  EXPECT_EQ(128u, SkColorGetA(decoded.getColor(3, 3)));
}

// --- hwui: the software Canvas, RenderNode ----------------------------------------------------

TEST(Hwui, SkiaCanvasDrawsIntoABitmap) {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(40, 30);
  std::unique_ptr<Canvas> canvas(Canvas::create_canvas(bitmap));
  ASSERT_TRUE(canvas != nullptr);
  EXPECT_EQ(40, canvas->width());
  canvas->drawColor(SK_ColorBLACK, SkXfermode::kSrc_Mode);
  SkPaint paint;
  paint.setColor(SK_ColorYELLOW);
  canvas->save(SkCanvas::kMatrixClip_SaveFlag);
  canvas->clipRect(0, 0, 20, 30, SkRegion::kIntersect_Op);
  canvas->drawRect(0, 0, 40, 30, paint);  // only the left half
  canvas->restore();
  EXPECT_EQ(SK_ColorYELLOW, PixelAt(bitmap, 10, 10));
  EXPECT_EQ(SK_ColorBLACK, PixelAt(bitmap, 30, 10));
  canvas->translate(25, 5);
  paint.setColor(SK_ColorCYAN);
  canvas->drawRect(0, 0, 5, 5, paint);
  EXPECT_EQ(SK_ColorCYAN, PixelAt(bitmap, 27, 7));
  EXPECT_EQ(1, canvas->getSaveCount());
}

TEST(Hwui, RenderNodeProperties) {
  sp<uirenderer::RenderNode> node = new uirenderer::RenderNode();
  node->setName("test.Node");
  EXPECT_STREQ("Node", node->getName());
  node->mutateStagingProperties().setAlpha(0.5f);
  node->mutateStagingProperties().setTranslationX(12.0f);
  node->mutateStagingProperties().setLeftTopRightBottom(0, 0, 100, 50);
  EXPECT_FLOAT_EQ(0.5f, node->stagingProperties().getAlpha());
  EXPECT_FLOAT_EQ(12.0f, node->stagingProperties().getTranslationX());
  EXPECT_EQ(100, node->stagingProperties().getWidth());
  // Recording a display list (what View.updateDisplayListIfDirty does).
  std::unique_ptr<uirenderer::DisplayListCanvas> recorder(new uirenderer::DisplayListCanvas());
  recorder->setViewport(100, 50);
  recorder->prepare();
  SkPaint paint;
  paint.setColor(SK_ColorRED);
  recorder->drawRect(0, 0, 10, 10, paint);
  uirenderer::DisplayListData* data = recorder->finishRecording();
  ASSERT_TRUE(data != nullptr);
  EXPECT_FALSE(data->isEmpty());
  node->setStagingDisplayList(data);
}

// --- text: minikin with Roboto through TypefaceImpl ----------------------------------------------

TypefaceImpl* LoadRoboto() {
  static TypefaceImpl* typeface = nullptr;
  if (typeface)
    return typeface;
  const char* kFonts[] = {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/Roboto-Bold.ttf"};
  std::vector<jlong> families;
  FontFamily* family = new FontFamily();
  for (const char* path : kFonts) {
    SkTypeface* face = SkTypeface::CreateFromFile(path);
    if (!face)
      return nullptr;
    MinikinFont* font = new MinikinFontSkia(face);
    family->addFont(font);
    font->Unref();
  }
  families.push_back(reinterpret_cast<jlong>(family));
  typeface = TypefaceImpl_createFromFamilies(families.data(), families.size());
  return typeface;
}

TEST(Text, FontManagerFindsTheSystemFonts) {
  SkAutoTUnref<SkFontMgr> mgr(SkFontMgr::RefDefault());
  ASSERT_TRUE(mgr.get() != nullptr);
  EXPECT_GE(mgr->countFamilies(), 3);  // sans-serif, sans-serif-condensed, monospace
  SkAutoTUnref<SkFontStyleSet> sans(mgr->matchFamily("sans-serif"));
  ASSERT_TRUE(sans.get() != nullptr);
  EXPECT_GE(sans->count(), 12);  // Roboto: 6 weights, upright and italic
  SkAutoTUnref<SkTypeface> mono(mgr->matchFamilyStyle("monospace", SkFontStyle()));
  ASSERT_TRUE(mono.get() != nullptr);
  SkAutoTUnref<SkTypeface> def(SkTypeface::RefDefault());
  ASSERT_TRUE(def.get() != nullptr);
  SkString name;
  def->getFamilyName(&name);
  EXPECT_STREQ("sans-serif", name.c_str());
}

TEST(Text, MinikinLayoutWithRoboto) {
  TypefaceImpl* typeface = LoadRoboto();
  ASSERT_TRUE(typeface != nullptr) << "Roboto in /system/fonts";
  Paint paint;
  paint.setTextSize(20);
  paint.setAntiAlias(true);
  const char16_t kHello[] = u"Hello, WebAssembly";
  const size_t n = sizeof(kHello) / sizeof(kHello[0]) - 1;
  Layout layout;
  MinikinUtils::doLayout(&layout, &paint, kBidi_LTR, typeface,
                         reinterpret_cast<const uint16_t*>(kHello), 0, n, n);
  EXPECT_EQ(n, layout.nGlyphs());  // Latin: one glyph per letter
  const float advance = layout.getAdvance();
  EXPECT_GT(advance, 100.0f);
  EXPECT_LT(advance, 250.0f);
  // "ii" is narrower than "WW" (proportional font).
  Layout narrow, wide;
  const uint16_t ii[] = {'i', 'i'}, ww[] = {'W', 'W'};
  MinikinUtils::doLayout(&narrow, &paint, kBidi_LTR, typeface, ii, 0, 2, 2);
  MinikinUtils::doLayout(&wide, &paint, kBidi_LTR, typeface, ww, 0, 2, 2);
  EXPECT_LT(narrow.getAdvance() * 2, wide.getAdvance());

  // Glyphs drawn through the font run (as android_graphics_Canvas does).
  SkBitmap bitmap;
  bitmap.allocN32Pixels(220, 30);
  bitmap.eraseColor(SK_ColorWHITE);
  std::unique_ptr<Canvas> canvas(Canvas::create_canvas(bitmap));
  std::vector<uint16_t> glyphs(layout.nGlyphs());
  std::vector<float> pos(layout.nGlyphs() * 2);
  for (size_t i = 0; i < layout.nGlyphs(); ++i) {
    glyphs[i] = layout.getGlyphId(i);
    pos[2 * i] = 2 + layout.getX(i);  // absolute, as android_graphics_Canvas passes them
    pos[2 * i + 1] = 22 + layout.getY(i);
  }
  SkPaint glyph_paint(paint);
  glyph_paint.setTextEncoding(SkPaint::kGlyphID_TextEncoding);
  glyph_paint.setColor(SK_ColorBLACK);
  MinikinFontSkia::populateSkPaint(&glyph_paint, static_cast<MinikinFontSkia*>(layout.getFont(0)),
                                   layout.getFakery(0));
  canvas->drawText(glyphs.data(), pos.data(), glyphs.size(), glyph_paint, 2, 22, 0, 0, advance,
                   30, advance);
  int ink = 0, ink_right = 0;
  for (int y = 0; y < 30; ++y) {
    for (int x = 0; x < 220; ++x) {
      if (SkColorGetR(bitmap.getColor(x, y)) < 128) {
        ++ink;
        if (x > advance + 4)
          ++ink_right;
      }
    }
  }
  EXPECT_GT(ink, 150);
  EXPECT_EQ(0, ink_right);
}

// --- libgui over the in-process SurfaceFlinger ----------------------------------------------------

uint32_t ArgbAt(int x, int y) {
  uint32_t argb = 0;
  EXPECT_TRUE(arcwasm::fw6::ReadDisplay(x, y, 1, 1, &argb));
  return argb;
}

void Fill(const sp<Surface>& surface, uint32_t rgba, int* out_w = nullptr, int* out_h = nullptr) {
  ANativeWindow_Buffer buffer;
  ASSERT_EQ(NO_ERROR, surface->lock(&buffer, nullptr));
  for (int y = 0; y < buffer.height; ++y) {
    uint32_t* row = static_cast<uint32_t*>(buffer.bits) + y * buffer.stride;
    std::fill(row, row + buffer.width, rgba);
  }
  if (out_w)
    *out_w = buffer.width;
  if (out_h)
    *out_h = buffer.height;
  ASSERT_EQ(NO_ERROR, surface->unlockAndPost());
}

TEST(Gui, DisplayInfo) {
  sp<IBinder> display = SurfaceComposerClient::getBuiltInDisplay(ISurfaceComposer::eDisplayIdMain);
  ASSERT_TRUE(display != nullptr);
  DisplayInfo info;
  ASSERT_EQ(NO_ERROR, SurfaceComposerClient::getDisplayInfo(display, &info));
  int32_t w, h;
  arcwasm::fw6::GetDisplaySize(&w, &h);
  EXPECT_EQ(static_cast<uint32_t>(w), info.w);
  EXPECT_EQ(static_cast<uint32_t>(h), info.h);
  EXPECT_FLOAT_EQ(60.0f, info.fps);
  EXPECT_GT(info.w, 100u);
}

TEST(Gui, SurfacesComposeOnTheDisplay) {
  sp<SurfaceComposerClient> client = new SurfaceComposerClient();
  ASSERT_EQ(NO_ERROR, client->initCheck());
  sp<SurfaceControl> a = client->createSurface(String8("A"), 100, 80, PIXEL_FORMAT_RGBA_8888, 0);
  sp<SurfaceControl> b = client->createSurface(String8("B"), 40, 40, PIXEL_FORMAT_RGBA_8888, 0);
  // A: (20,10)-(120,90); B: (100,60)-(140,100), half over A.
  ASSERT_TRUE(a != nullptr && a->isValid());
  ASSERT_TRUE(b != nullptr && b->isValid());
  SurfaceComposerClient::openGlobalTransaction();
  a->setLayer(10);
  a->setPosition(20, 10);
  a->show();
  b->setLayer(20);
  b->setPosition(100, 60);
  b->setAlpha(0.5f);
  b->show();
  SurfaceComposerClient::closeGlobalTransaction();

  int w = 0, h = 0;
  Fill(a->getSurface(), 0xff0000ffu, &w, &h);  // RGBA bytes: opaque red
  EXPECT_EQ(100, w);
  EXPECT_EQ(80, h);
  Fill(b->getSurface(), 0xffff0000u);  // opaque blue at half alpha
  EXPECT_EQ(0xffff0000u, ArgbAt(25, 15));  // A alone
  EXPECT_EQ(0xff000000u, ArgbAt(5, 5));    // nothing: black
  const uint32_t mixed = ArgbAt(110, 70);  // B half over A
  EXPECT_NEAR(128, (mixed >> 16) & 0xff, 2);
  EXPECT_NEAR(127, mixed & 0xff, 2);
  const uint32_t over_black = ArgbAt(130, 95);  // B half over black (outside A)
  EXPECT_EQ(0u, (over_black >> 16) & 0xff);
  EXPECT_NEAR(127, over_black & 0xff, 2);

  // A new frame replaces the old one; hiding and resizing.
  Fill(a->getSurface(), 0xff00ff00u);  // green
  EXPECT_EQ(0xff00ff00u, ArgbAt(25, 15));
  SurfaceComposerClient::openGlobalTransaction();
  b->hide();
  a->setSize(50, 20);
  SurfaceComposerClient::closeGlobalTransaction();
  EXPECT_EQ(0xff00ff00u, ArgbAt(60, 25));  // A's old buffer cut to 50x20
  EXPECT_EQ(0xff000000u, ArgbAt(25, 40));
  EXPECT_EQ(0xff000000u, ArgbAt(130, 95));  // B hidden
  Fill(a->getSurface(), 0xff00ffffu, &w, &h);  // yellow, at the new size
  EXPECT_EQ(50, w);
  EXPECT_EQ(20, h);
  EXPECT_EQ(0xffffff00u, ArgbAt(25, 15));
  String8 layers = arcwasm::fw6::DumpLayers();
  EXPECT_NE(nullptr, strstr(layers.string(), "A z=10 pos=20,10 size=50x20")) << layers.string();
  EXPECT_NE(nullptr, strstr(layers.string(), "hidden")) << layers.string();
  a->clear();
  b->clear();
  EXPECT_EQ(0xff000000u, ArgbAt(25, 15));
}

TEST(Gui, SurfaceCopiesBackOutsideTheDirtyRect) {
  sp<SurfaceComposerClient> client = new SurfaceComposerClient();
  sp<SurfaceControl> c = client->createSurface(String8("C"), 60, 60, PIXEL_FORMAT_RGBX_8888, 0);
  SurfaceComposerClient::openGlobalTransaction();
  c->setLayer(5);
  c->setPosition(0, 0);
  c->show();
  SurfaceComposerClient::closeGlobalTransaction();
  sp<Surface> surface = c->getSurface();
  Fill(surface, 0xff0000ffu);  // red, everything
  Fill(surface, 0xff0000ffu);  // both buffers have been drawn once
  ANativeWindow_Buffer buffer;
  ARect dirty = {10, 10, 20, 20};
  ASSERT_EQ(NO_ERROR, surface->lock(&buffer, &dirty));
  for (int y = dirty.top; y < dirty.bottom; ++y) {
    uint32_t* row = static_cast<uint32_t*>(buffer.bits) + y * buffer.stride;
    std::fill(row + dirty.left, row + dirty.right, 0xffff0000u);
  }
  ASSERT_EQ(NO_ERROR, surface->unlockAndPost());
  EXPECT_EQ(0xff0000ffu, ArgbAt(15, 15));  // the dirty part: blue
  EXPECT_EQ(0xffff0000u, ArgbAt(40, 40));  // copied back: red
  c->clear();
}

TEST(Gui, Screenshot) {
  sp<SurfaceComposerClient> client = new SurfaceComposerClient();
  sp<SurfaceControl> s = client->createSurface(String8("S"), 30, 30, PIXEL_FORMAT_RGBA_8888, 0);
  SurfaceComposerClient::openGlobalTransaction();
  s->setLayer(7);
  s->setPosition(10, 10);
  s->show();
  SurfaceComposerClient::closeGlobalTransaction();
  Fill(s->getSurface(), 0xff00ff00u);
  sp<IBinder> display = SurfaceComposerClient::getBuiltInDisplay(ISurfaceComposer::eDisplayIdMain);
  ScreenshotClient shot;
  ASSERT_EQ(NO_ERROR, shot.update(display, Rect(0, 0, 60, 60), false));
  EXPECT_EQ(60u, shot.getWidth());
  const uint32_t* pixels = static_cast<const uint32_t*>(shot.getPixels());
  ASSERT_TRUE(pixels != nullptr);
  EXPECT_EQ(0xff00ff00u, pixels[20 * shot.getStride() + 20]);  // RGBA bytes: green
  EXPECT_EQ(0xff000000u, pixels[5 * shot.getStride() + 5]);
  s->clear();
}

TEST(Gui, Vsync) {
  DisplayEventReceiver receiver;
  ASSERT_EQ(NO_ERROR, receiver.initCheck());
  DisplayEventReceiver::Event events[4];
  nsecs_t last = 0;
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(NO_ERROR, receiver.requestNextVsync());
    struct pollfd p = {receiver.getFd(), POLLIN, 0};
    ASSERT_EQ(1, poll(&p, 1, 1000));
    const ssize_t n = receiver.getEvents(events, 4);
    ASSERT_GE(n, 1);
    EXPECT_EQ(DisplayEventReceiver::DISPLAY_EVENT_VSYNC, events[n - 1].header.type);
    const nsecs_t t = events[n - 1].header.timestamp;
    if (last)
      EXPECT_EQ(0, (t - last) % 16666666) << "on the 60 Hz grid";
    last = t;
  }
}

// --- libinput -------------------------------------------------------------------------------------

TEST(Input, ChannelCarriesMotionAndKeyEvents) {
  sp<InputChannel> server, client;
  ASSERT_EQ(OK, InputChannel::openInputChannelPair(String8("test"), server, client));
  InputPublisher publisher(server);
  InputConsumer consumer(client);
  PointerProperties props;
  props.clear();
  props.id = 0;
  props.toolType = AMOTION_EVENT_TOOL_TYPE_FINGER;
  PointerCoords coords;
  coords.clear();
  coords.setAxisValue(AMOTION_EVENT_AXIS_X, 12.5f);
  coords.setAxisValue(AMOTION_EVENT_AXIS_Y, 34.0f);
  coords.setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, 1.0f);
  const nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
  ASSERT_EQ(OK, publisher.publishMotionEvent(1, 2, AINPUT_SOURCE_TOUCHSCREEN,
                                             AMOTION_EVENT_ACTION_DOWN, 0, 0, 0, 0, 0, -10, -4,
                                             1, 1, now, now, 1, &props, &coords));
  ASSERT_EQ(OK, publisher.publishKeyEvent(2, 1, AINPUT_SOURCE_KEYBOARD, AKEY_EVENT_ACTION_DOWN, 0,
                                          AKEYCODE_A, 30, 0, 0, now, now));
  PreallocatedInputEventFactory factory;
  uint32_t seq;
  InputEvent* event = nullptr;
  ASSERT_EQ(OK, consumer.consume(&factory, true, -1, &seq, &event));
  ASSERT_EQ(AINPUT_EVENT_TYPE_MOTION, event->getType());
  MotionEvent* motion = static_cast<MotionEvent*>(event);
  EXPECT_EQ(1u, seq);
  EXPECT_EQ(AMOTION_EVENT_ACTION_DOWN, motion->getAction());
  EXPECT_FLOAT_EQ(2.5f, motion->getX(0));  // with the offset
  EXPECT_FLOAT_EQ(12.5f, motion->getRawX(0));
  EXPECT_FLOAT_EQ(30.0f, motion->getY(0));
  ASSERT_EQ(OK, consumer.consume(&factory, true, -1, &seq, &event));
  ASSERT_EQ(AINPUT_EVENT_TYPE_KEY, event->getType());
  EXPECT_EQ(AKEYCODE_A, static_cast<KeyEvent*>(event)->getKeyCode());
  EXPECT_EQ(WOULD_BLOCK, consumer.consume(&factory, true, -1, &seq, &event));
  ASSERT_EQ(OK, consumer.sendFinishedSignal(1, true));
  ASSERT_EQ(OK, consumer.sendFinishedSignal(2, false));
  uint32_t done;
  bool handled;
  ASSERT_EQ(OK, publisher.receiveFinishedSignal(&done, &handled));
  EXPECT_EQ(1u, done);
  EXPECT_TRUE(handled);
  ASSERT_EQ(OK, publisher.receiveFinishedSignal(&done, &handled));
  EXPECT_EQ(2u, done);
  EXPECT_FALSE(handled);
}

}  // namespace

int main(int argc, char** argv) {
  arc_art_boot_kernel();
  // As artvm does (art/artvm.cc) and init on a device: Skia's font manager
  // finds the fonts of /system/etc/fonts.xml under $ANDROID_ROOT/fonts/.
  // Without it no family loads, and SkFontMgr_Android indexes an empty
  // array: undefined behaviour that crashed or not with the memory layout
  // (the length of the program's path).
  setenv("ANDROID_ROOT", "/system", 0);
  // The image codecs register themselves in static initializers; this keeps
  // their objects (as GraphicsJNI's registration does).
  SkForceLinking(false);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
