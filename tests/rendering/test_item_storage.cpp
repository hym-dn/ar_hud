/**
 * @file test_item_storage.cpp
 * @brief ItemStorage 树形管理单测（ADR-0005）
 *
 * 覆盖语义：
 *   - 无父 item 的有效状态 = 自身状态（向后兼容基线）
 *   - transform / opacity 沿父链连乘，visible 沿父链 AND
 *   - z_as_relative：相对父链累加（默认 true）/ 绝对（false）/ 三级累加
 *   - Text：位置经父链矩阵变换、alpha 沿链连乘、跨类型挂接拒绝
 *   - SetParent：环拒绝、自挂接拒绝、无效父 RID 视为根、kInvalid 摘到根
 *   - Free 不级联：Canvas/Spatial 子与 Text 子自动摘到根
 *
 * ItemStorage 为纯状态存储（无 GPU 依赖），可直接构造测试。
 */

#include <gtest/gtest.h>

#include "storage/item_storage.h"

namespace {
using arhud::CanvasItemData;
using arhud::ItemStorage;
using arhud::RID;
using arhud::SpatialItemData;
using arhud::TextItemData;

constexpr float kEps = 1e-5f;

/// @brief 构造平移矩阵（列主序 float[16]，先缩放旋转后平移约定的纯平移）
void MakeTranslation(float p_tx, float p_ty, float p_out[16]) {
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  for (int i = 0; i < 16; ++i) {
    p_out[i] = identity[i];
  }
  p_out[12] = p_tx;  // col 3, row 0
  p_out[13] = p_ty;  // col 3, row 1
}

TEST(ItemStorageTree, NoParentEffectiveEqualsOwnState) {
  ItemStorage storage;
  RID item = storage.CanvasItemCreate();

  float local[16];
  MakeTranslation(3.0f, 4.0f, local);
  storage.CanvasItemSetTransform(item, local);
  storage.CanvasItemSetOpacity(item, 0.7f);
  storage.CanvasItemSetZOrder(item, 9);

  ItemStorage::CanvasItemEffectiveState eff;
  ASSERT_TRUE(storage.CanvasItemGetEffective(item, eff));
  EXPECT_NEAR(eff.transform[12], 3.0f, kEps);
  EXPECT_NEAR(eff.transform[13], 4.0f, kEps);
  EXPECT_NEAR(eff.opacity, 0.7f, kEps);
  EXPECT_EQ(eff.z_order, 9);
  EXPECT_TRUE(eff.visible);
}

TEST(ItemStorageTree, TransformChainMultiplies) {
  ItemStorage storage;
  RID parent = storage.CanvasItemCreate();
  RID child = storage.CanvasItemCreate();

  float parent_local[16];
  MakeTranslation(10.0f, 20.0f, parent_local);
  storage.CanvasItemSetTransform(parent, parent_local);

  float child_local[16];
  MakeTranslation(1.0f, 2.0f, child_local);
  storage.CanvasItemSetTransform(child, child_local);
  storage.CanvasItemSetParent(child, parent);

  ItemStorage::CanvasItemEffectiveState eff;
  ASSERT_TRUE(storage.CanvasItemGetEffective(child, eff));
  // world = parent × child = 平移 (11, 22)
  EXPECT_NEAR(eff.transform[12], 11.0f, kEps);
  EXPECT_NEAR(eff.transform[13], 22.0f, kEps);
}

TEST(ItemStorageTree, VisibilityPropagationAND) {
  ItemStorage storage;
  RID parent = storage.CanvasItemCreate();
  RID child = storage.CanvasItemCreate();
  storage.CanvasItemSetParent(child, parent);

  storage.CanvasItemSetVisible(child, true);
  ItemStorage::CanvasItemEffectiveState eff0;
  EXPECT_TRUE(storage.CanvasItemGetEffective(child, eff0));
  EXPECT_TRUE(eff0.visible);

  // 父隐藏 → 子树内不可见（子自身仍 visible=true）
  storage.CanvasItemSetVisible(parent, false);
  ItemStorage::CanvasItemEffectiveState eff;
  ASSERT_TRUE(storage.CanvasItemGetEffective(child, eff));
  EXPECT_TRUE(storage.CanvasItemIsValid(child));
  EXPECT_FALSE(eff.visible);

  // 父恢复 → 子恢复可见
  storage.CanvasItemSetVisible(parent, true);
  ASSERT_TRUE(storage.CanvasItemGetEffective(child, eff));
  EXPECT_TRUE(eff.visible);
}

TEST(ItemStorageTree, OpacityChainMultiplies) {
  ItemStorage storage;
  RID parent = storage.CanvasItemCreate();
  RID child = storage.CanvasItemCreate();
  storage.CanvasItemSetParent(child, parent);

  storage.CanvasItemSetOpacity(parent, 0.5f);
  storage.CanvasItemSetOpacity(child, 0.5f);

  ItemStorage::CanvasItemEffectiveState eff;
  ASSERT_TRUE(storage.CanvasItemGetEffective(child, eff));
  EXPECT_NEAR(eff.opacity, 0.25f, kEps);
}

TEST(ItemStorageTree, ZRelativeAccumulatesAlongChain) {
  ItemStorage storage;
  RID root = storage.CanvasItemCreate();
  RID mid = storage.CanvasItemCreate();
  RID leaf = storage.CanvasItemCreate();
  storage.CanvasItemSetParent(mid, root);
  storage.CanvasItemSetParent(leaf, mid);

  storage.CanvasItemSetZOrder(root, 10);
  storage.CanvasItemSetZOrder(mid, 1);
  storage.CanvasItemSetZOrder(leaf, 2);
  // 默认 z_as_relative = true：10 + 1 + 2
  ItemStorage::CanvasItemEffectiveState eff;
  ASSERT_TRUE(storage.CanvasItemGetEffective(leaf, eff));
  EXPECT_EQ(eff.z_order, 13);

  // mid 切绝对：leaf 仍相对 mid，mid 不再累加 root
  storage.CanvasItemSetZAsRelative(mid, false);
  ASSERT_TRUE(storage.CanvasItemGetEffective(mid, eff));
  EXPECT_EQ(eff.z_order, 1);
  ASSERT_TRUE(storage.CanvasItemGetEffective(leaf, eff));
  EXPECT_EQ(eff.z_order, 3);
}

TEST(ItemStorageTree, CycleAndSelfParentRejected) {
  ItemStorage storage;
  RID a = storage.CanvasItemCreate();
  RID b = storage.CanvasItemCreate();
  storage.CanvasItemSetParent(b, a);  // B → A，合法

  // A → B 将成环（A→B→A），拒绝后 A 保持根
  storage.CanvasItemSetParent(a, b);
  ItemStorage::CanvasItemEffectiveState eff;
  ASSERT_TRUE(storage.CanvasItemGetEffective(a, eff));

  // 自挂接拒绝
  storage.CanvasItemSetParent(a, a);
  ASSERT_TRUE(storage.CanvasItemGetEffective(a, eff));

  // 无效父 RID 视为根（防御，不崩溃）
  storage.CanvasItemSetParent(a, RID(99999));
  ASSERT_TRUE(storage.CanvasItemGetEffective(a, eff));
}

TEST(ItemStorageTree, FreeDetachesChildrenNotCascade) {
  ItemStorage storage;
  RID parent = storage.CanvasItemCreate();
  RID canvas_child = storage.CanvasItemCreate();
  RID text_child = storage.TextItemCreate();
  storage.CanvasItemSetParent(canvas_child, parent);
  storage.TextItemSetParent(text_child, parent);

  storage.CanvasItemFree(parent);

  // 子存活且摘到根（Free 不级联）
  EXPECT_TRUE(storage.CanvasItemIsValid(canvas_child));
  EXPECT_TRUE(storage.TextItemIsValid(text_child));
  ItemStorage::CanvasItemEffectiveState eff;
  ASSERT_TRUE(storage.CanvasItemGetEffective(canvas_child, eff));
  ItemStorage::TextItemEffectiveState teff;
  ASSERT_TRUE(storage.TextItemGetEffective(text_child, teff));
}

TEST(ItemStorageTree, SetParentToInvalidDetaches) {
  ItemStorage storage;
  RID parent = storage.CanvasItemCreate();
  RID child = storage.CanvasItemCreate();
  storage.CanvasItemSetParent(child, parent);
  storage.CanvasItemSetParent(child, RID());  // kInvalid = 摘到根

  float local[16];
  MakeTranslation(5.0f, 0.0f, local);
  storage.CanvasItemSetTransform(parent, local);

  // 摘除后父移动不影响子
  ItemStorage::CanvasItemEffectiveState eff;
  ASSERT_TRUE(storage.CanvasItemGetEffective(child, eff));
  EXPECT_NEAR(eff.transform[12], 0.0f, kEps);
}

TEST(ItemStorageTree, TextPositionAndAlphaThroughParentChain) {
  ItemStorage storage;
  RID panel = storage.CanvasItemCreate();
  RID text = storage.TextItemCreate();
  storage.CanvasItemSetOpacity(panel, 0.5f);

  float panel_local[16];
  MakeTranslation(10.0f, 0.0f, panel_local);
  storage.CanvasItemSetTransform(panel, panel_local);

  storage.TextItemSetPosition(text, 2.0f, 3.0f);
  const float color[4] = {1.0f, 1.0f, 1.0f, 0.8f};
  storage.TextItemSetColor(text, color);
  storage.TextItemSetParent(text, panel);

  ItemStorage::TextItemEffectiveState eff;
  ASSERT_TRUE(storage.TextItemGetEffective(text, eff));
  // 位置 = 父链矩阵 × (2, 3) = (12, 3)
  EXPECT_NEAR(eff.x, 12.0f, kEps);
  EXPECT_NEAR(eff.y, 3.0f, kEps);
  // alpha = 父链 opacity × 自身 color[3] = 0.5 × 0.8
  EXPECT_NEAR(eff.alpha, 0.4f, kEps);
  EXPECT_TRUE(eff.visible);
}

TEST(ItemStorageTree, TextCrossTypeParentRejected) {
  ItemStorage storage;
  RID spatial = storage.SpatialItemCreate();
  RID text = storage.TextItemCreate();

  // Text 的父必须是 Canvas item；挂 Spatial 被拒绝 → 视为根
  storage.TextItemSetParent(text, spatial);
  ItemStorage::TextItemEffectiveState eff;
  ASSERT_TRUE(storage.TextItemGetEffective(text, eff));
  EXPECT_NEAR(eff.x, 0.0f, kEps);  // 自身状态，未被父链变换
  EXPECT_NEAR(eff.alpha, 1.0f, kEps);
}

TEST(ItemStorageTree, TextZRelative) {
  ItemStorage storage;
  RID panel = storage.CanvasItemCreate();
  RID text = storage.TextItemCreate();
  storage.CanvasItemSetZOrder(panel, 4);
  storage.TextItemSetZOrder(text, 1);
  storage.TextItemSetParent(text, panel);

  ItemStorage::TextItemEffectiveState eff;
  ASSERT_TRUE(storage.TextItemGetEffective(text, eff));
  EXPECT_EQ(eff.z_order, 5);  // 默认相对累加

  storage.TextItemSetZAsRelative(text, false);
  ASSERT_TRUE(storage.TextItemGetEffective(text, eff));
  EXPECT_EQ(eff.z_order, 1);
}

TEST(ItemStorageTree, SpatialTransformChain) {
  ItemStorage storage;
  RID parent = storage.SpatialItemCreate();
  RID child = storage.SpatialItemCreate();

  float parent_local[16];
  MakeTranslation(100.0f, 0.0f, parent_local);
  storage.SpatialItemSetTransform(parent, parent_local);

  float child_local[16];
  MakeTranslation(0.0f, 7.0f, child_local);
  storage.SpatialItemSetTransform(child, child_local);
  storage.SpatialItemSetParent(child, parent);

  float world[16];
  ASSERT_TRUE(storage.SpatialItemGetEffectiveTransform(child, world));
  EXPECT_NEAR(world[12], 100.0f, kEps);
  EXPECT_NEAR(world[13], 7.0f, kEps);
}

// ── 审视回归（2026-09-19 评审修复）：传播性变更的屏级 dirty 路由 ──

TEST(ItemStorageTree, DirtyRouting_VisiblePropagatesToChildScreens) {
  // 父在屏 0 可见、子在屏 1 可见（掩码不同）：父隐藏时屏 1 必须标脏，
  // 否则子所在屏不重渲 → 陈旧帧（评审发现的 BUG A）
  ItemStorage storage;
  RID parent = storage.CanvasItemCreate();
  RID child = storage.CanvasItemCreate();
  storage.CanvasItemSetParent(child, parent);
  storage.CanvasItemSetVisibilityMask(parent, 0x1);
  storage.CanvasItemSetVisibilityMask(child, 0x2);
  storage.ClearDirty();

  storage.CanvasItemSetVisible(parent, false);
  EXPECT_TRUE(storage.IsDirty());
  EXPECT_TRUE(storage.IsScreenDirty(0));  // 父自身所在屏
  EXPECT_TRUE(storage.IsScreenDirty(1));  // 子所在屏（子树掩码并集路由）
}

TEST(ItemStorageTree, DirtyRouting_TransformPropagatesToTextChildScreen) {
  // 爻项移动 → 挂在屏 1 的 Text 子孙所在屏也要标脏
  ItemStorage storage;
  RID panel = storage.CanvasItemCreate();
  RID text = storage.TextItemCreate();
  storage.TextItemSetParent(text, panel);
  storage.CanvasItemSetVisibilityMask(panel, 0x1);
  storage.TextItemSetVisibilityMask(text, 0x2);
  storage.ClearDirty();

  float local[16];
  MakeTranslation(10.0f, 0.0f, local);
  storage.CanvasItemSetTransform(panel, local);
  EXPECT_TRUE(storage.IsScreenDirty(0));
  EXPECT_TRUE(storage.IsScreenDirty(1));  // Text 子孙所在屏
}

TEST(ItemStorageTree, DirtyRouting_SpatialSubtree) {
  ItemStorage storage;
  RID parent = storage.SpatialItemCreate();
  RID child = storage.SpatialItemCreate();
  storage.SpatialItemSetParent(child, parent);
  storage.SpatialItemSetVisibilityMask(parent, 0x1);
  storage.SpatialItemSetVisibilityMask(child, 0x2);
  storage.ClearDirty();

  float local[16];
  MakeTranslation(5.0f, 0.0f, local);
  storage.SpatialItemSetTransform(parent, local);
  EXPECT_TRUE(storage.IsScreenDirty(0));
  EXPECT_TRUE(storage.IsScreenDirty(1));
}

TEST(ItemStorageTree, SetParentInvalidItemStillDirties) {
  // 文件头约定：Set* 无效 RID 仍路由屏位（防丢帧）——评审发现的约定不一致（BUG
  // B）
  ItemStorage storage;
  storage.ClearDirty();
  storage.CanvasItemSetParent(RID(99999), RID());
  EXPECT_TRUE(storage.IsDirty());
}

// ── Create 参数化掩码（2026-09-19：创建即带归属，跳过"全屏创建→收窄"绕路）──

TEST(ItemStorageTree, CreateWithNarrowMaskRoutesOnlyThoseScreens) {
  ItemStorage storage;
  storage.ClearDirty();
  RID item = storage.CanvasItemCreate(0x2);  // 只挂屏 1
  EXPECT_TRUE(item.IsValid());
  EXPECT_TRUE(storage.IsDirty());
  EXPECT_TRUE(storage.IsScreenDirty(1));
  EXPECT_FALSE(storage.IsScreenDirty(0));  // 屏 0 不再陪跑
}

TEST(ItemStorageTree, CreateDefaultMaskKeepsAllScreensDirty) {
  // 默认参数向后兼容：无参创建 = 全屏归属 = 全屏标脏（既有行为）
  ItemStorage storage;
  storage.ClearDirty();
  RID item = storage.CanvasItemCreate();
  EXPECT_TRUE(storage.IsScreenDirty(0));
  EXPECT_TRUE(storage.IsScreenDirty(1));
}

TEST(ItemStorageTree, CreateNarrowMaskSpatialAndText) {
  ItemStorage storage;
  storage.ClearDirty();
  RID s = storage.SpatialItemCreate(0x4);  // 屏 2
  RID t = storage.TextItemCreate(0x8);     // 屏 3
  EXPECT_TRUE(s.IsValid());
  EXPECT_TRUE(t.IsValid());
  EXPECT_TRUE(storage.IsScreenDirty(2));
  EXPECT_TRUE(storage.IsScreenDirty(3));
  EXPECT_FALSE(storage.IsScreenDirty(0));
}
}  // namespace
