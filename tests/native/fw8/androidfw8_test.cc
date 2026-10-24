// libandroidfw 8.1 on framework-res.apk 8.1 (/system/framework, built with
// aapt2 by mk/topic-fw8-core.mk): the AssetManager/ResTable that
// android.content.res uses in 8.1, and AssetManager2/ApkAssets/LoadedArsc,
// new in 8.0 (aapt2's tools and 9.0's framework use them). Part of make
// fw8-native-test. The ids are android.R's (public.xml) and
// com.android.internal.R's of that build.

#include <androidfw/ApkAssets.h>
#include <androidfw/Asset.h>
#include <androidfw/AssetManager.h>
#include <androidfw/AssetManager2.h>
#include <androidfw/ResourceTypes.h>
#include <androidfw/ZipFileRO.h>
#include <utils/String16.h>
#include <utils/String8.h>

#include <memory>
#include <string>

#include "gtest/gtest.h"

using namespace android;

namespace {

const char kFrameworkRes[] = "/system/framework/framework-res.apk";
const uint32_t kStringOk = 0x0104000a;            // android.R.string.ok
const uint32_t kAttrColorBackground = 0x01010031;  // android.R.attr.colorBackground
const uint32_t kStyleThemeMaterialLight = 0x01030237;
const uint32_t kLayoutSimpleListItem1 = 0x01090003;
const uint32_t kIntegerShortAnimTime = 0x010e0000;  // com.android.internal.R.integer

std::string Utf8(const char16_t* s, size_t len) {
  return std::string(String8(s, len).string());
}

TEST(ZipFileRO8, OpensFrameworkRes) {
  std::unique_ptr<ZipFileRO> zip(ZipFileRO::open(kFrameworkRes));
  ASSERT_TRUE(zip != nullptr);
  ZipEntryRO arsc = zip->findEntryByName("resources.arsc");
  ASSERT_TRUE(arsc != nullptr);
  uint32_t size = 0;
  ASSERT_TRUE(zip->getEntryInfo(arsc, nullptr, &size, nullptr, nullptr, nullptr, nullptr));
  EXPECT_GT(size, 1000000u);
  zip->releaseEntry(arsc);
  EXPECT_TRUE(zip->findEntryByName("AndroidManifest.xml") != nullptr);
}

// --- AssetManager / ResTable (what android.content.res.AssetManager uses in 8.1) ---

class AssetManager8 : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(am_.addAssetPath(String8(kFrameworkRes), &cookie_, false, true));
    ResTable_config config;
    memset(&config, 0, sizeof(config));
    config.sdkVersion = 27;
    am_.setConfiguration(config);
  }
  AssetManager am_;
  int32_t cookie_ = 0;
};

TEST_F(AssetManager8, FrameworkIsCookieOne) {
  EXPECT_EQ(1, cookie_);
  const ResTable& res = am_.getResources();
  EXPECT_EQ(NO_ERROR, res.getError());
  EXPECT_EQ(1u, res.getBasePackageCount());
  EXPECT_EQ(String16("android"), res.getBasePackageName(0));
  EXPECT_EQ(1u, res.getBasePackageId(0));
}

TEST_F(AssetManager8, StringByIdAndName) {
  const ResTable& res = am_.getResources();
  Res_value v;
  ssize_t block = res.getResource(kStringOk, &v, false);
  ASSERT_GE(block, 0);
  ASSERT_EQ(Res_value::TYPE_STRING, v.dataType);
  size_t len = 0;
  const char16_t* s = res.getTableStringBlock(block)->stringAt(v.data, &len);
  ASSERT_TRUE(s != nullptr);
  EXPECT_EQ("OK", Utf8(s, len));
  const String16 name("ok"), type("string"), pkg("android");
  EXPECT_EQ(kStringOk, res.identifierForName(name.string(), name.size(), type.string(),
                                             type.size(), pkg.string(), pkg.size()));
  ResTable::resource_name rn;
  ASSERT_TRUE(res.getResourceName(kStringOk, true, &rn));
  EXPECT_EQ("android", Utf8(rn.package, rn.packageLen));
  // aapt2 keeps the key strings in UTF-8: the name comes as name8.
  ASSERT_TRUE(rn.name8 != nullptr || rn.name != nullptr);
  EXPECT_EQ("ok", rn.name8 ? std::string(rn.name8, rn.nameLen) : Utf8(rn.name, rn.nameLen));
}

TEST_F(AssetManager8, PrivateIntegerAndLayout) {
  const ResTable& res = am_.getResources();
  Res_value v;
  ASSERT_GE(res.getResource(kIntegerShortAnimTime, &v, false), 0);
  EXPECT_EQ(Res_value::TYPE_INT_DEC, v.dataType);
  EXPECT_EQ(200u, v.data);
  ASSERT_GE(res.getResource(kLayoutSimpleListItem1, &v, false), 0);
  ASSERT_EQ(Res_value::TYPE_STRING, v.dataType);  // the file's path in the apk
  Asset* layout = am_.openNonAsset(cookie_, "res/layout/simple_list_item_1.xml",
                                   Asset::ACCESS_BUFFER);
  ASSERT_TRUE(layout != nullptr);
  ResXMLTree tree;
  ASSERT_EQ(NO_ERROR, tree.setTo(layout->getBuffer(true), layout->getLength(), true));
  ResXMLParser::event_code_t ev;
  while ((ev = tree.next()) != ResXMLParser::START_TAG && ev != ResXMLParser::END_DOCUMENT) {
  }
  ASSERT_EQ(ResXMLParser::START_TAG, ev);
  size_t len = 0;
  EXPECT_EQ("TextView", Utf8(tree.getElementName(&len), len));
  delete layout;
}

TEST_F(AssetManager8, ThemeMaterialLight) {
  const ResTable& res = am_.getResources();
  ResTable::Theme theme(res);
  ASSERT_EQ(NO_ERROR, theme.applyStyle(kStyleThemeMaterialLight));
  Res_value v;
  uint32_t flags = 0;
  ssize_t block = theme.getAttribute(kAttrColorBackground, &v, &flags);
  ASSERT_GE(block, 0);
  block = res.resolveReference(&v, block, nullptr, &flags);
  ASSERT_GE(block, 0);
  ASSERT_GE(v.dataType, Res_value::TYPE_FIRST_COLOR_INT);
  EXPECT_EQ(0xfffafafau, v.data);  // @color/background_material_light = material_grey_50
}

// --- AssetManager2 / ApkAssets / LoadedArsc (8.0) ---------------------------------------

class AssetManager2_8 : public ::testing::Test {
 protected:
  void SetUp() override {
    apk_ = ApkAssets::Load(kFrameworkRes, true /*system*/);
    ASSERT_TRUE(apk_ != nullptr);
    ASSERT_TRUE(am_.SetApkAssets({apk_.get()}));
    ResTable_config config;
    memset(&config, 0, sizeof(config));
    config.sdkVersion = 27;
    am_.SetConfiguration(config);
  }
  std::unique_ptr<const ApkAssets> apk_;
  AssetManager2 am_;
};

TEST_F(AssetManager2_8, StringAndNames) {
  Res_value v;
  ResTable_config selected;
  uint32_t flags = 0;
  ApkAssetsCookie cookie = am_.GetResource(kStringOk, false, 0, &v, &selected, &flags);
  ASSERT_NE(kInvalidCookie, cookie);
  ASSERT_EQ(Res_value::TYPE_STRING, v.dataType);
  size_t len = 0;
  const char16_t* s = am_.GetStringPoolForCookie(cookie)->stringAt(v.data, &len);
  ASSERT_TRUE(s != nullptr);
  EXPECT_EQ("OK", Utf8(s, len));
  EXPECT_EQ(kStringOk, am_.GetResourceId("android:string/ok"));
  AssetManager2::ResourceName name;
  ASSERT_TRUE(am_.GetResourceName(kStringOk, &name));
  EXPECT_EQ("android", std::string(name.package, name.package_len));
}

TEST_F(AssetManager2_8, ThemeMaterialLight) {
  std::unique_ptr<Theme> theme = am_.NewTheme();
  ASSERT_TRUE(theme->ApplyStyle(kStyleThemeMaterialLight));
  Res_value v;
  uint32_t flags = 0;
  ApkAssetsCookie cookie = theme->GetAttribute(kAttrColorBackground, &v, &flags);
  ASSERT_NE(kInvalidCookie, cookie);
  ResTable_config config;
  uint32_t last_ref = 0;
  cookie = am_.ResolveReference(cookie, &v, &config, &flags, &last_ref);
  ASSERT_NE(kInvalidCookie, cookie);
  ASSERT_GE(v.dataType, Res_value::TYPE_FIRST_COLOR_INT);
  EXPECT_EQ(0xfffafafau, v.data);
}

}  // namespace
