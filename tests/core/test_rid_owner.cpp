/**
 * @file test_rid_owner.cpp
 * @brief RIDOwner 分配语义回归测试——世代/表标识校验、空闲槽复用、跨表唯一性。
 *
 * @par 为什么单独成文（而非混在渲染测试里）
 *   RID 是全引擎最底层的地基：RD 的 9 张资源表都建在它上面。两个被
 *   resize 故障暴露出来的性质必须有测试钉住：
 *   1. **复用槽位必须命中被释放的那一个**。历史 bug：MakeRid 从空闲链表
 *      取出 1-based 槽号后写 `entries_[index]`，而数组是 0-based——于是
 *      改写了**邻居**槽位、并把新 RID 指向一个从未置有效的槽，
 *      症状是"释放一个、坏掉两个"（off-by-one 的直接受害者）。
 *   2. **不同表的 RID 数值不得相同**。历史 bug：RID 只编了
 *      世代 + 本地槽号，没有表标识位，而本地槽号在每张表里都是
 *      1,2,3…；RD 的级联释放 `FreeDriverResourceByRid()` 靠"逐表 Owns()
 *      探测"判定资源类型，同值 RID 会让它把纹理的依赖边认成 Buffer
 *      并误杀活对象（症状：resize 后 `BufferUpdate: invalid buffer RID`
 *      与 `DrawListBegin invalid render pass` 成百出现）。
 *
 * @author yameng.he
 * @version 1.0
 * @date 2026-11-08
 * @copyright Copyright (c) 2026 ARHud Project
 * @ingroup tests
 */

#include "template/rid.h"

#include <gtest/gtest.h>

#include "template/local_vector.h"

namespace arhud {
namespace {

struct ProbeA {
  int value = 0;
};

struct ProbeB {
  int value = 0;
};

/**
 * @brief 检查一组 RID 数值是否有重复（线性扫描；用例规模小）
 *
 * @param[in] p_ids 待检的 RID 数值集合
 * @return 首个重复值；无重复返回 0
 */
uint64 FirstDuplicate(const LocalVector<uint64>& p_ids) {
  for (uint32 i = 0; i < p_ids.Size(); ++i) {
    for (uint32 j = i + 1; j < p_ids.Size(); ++j) {
      if (p_ids[i] == p_ids[j]) {
        return p_ids[i];
      }
    }
  }
  return 0;
}

// ═══════════════════════════════════════════════════════════════════════
// 槽位复用与邻居安全
// ═══════════════════════════════════════════════════════════════════════

TEST(RidOwnerTest, MakeRidReturnsDistinctValidRids) {
  RIDOwner<ProbeA> owner;

  ProbeA a0;
  ProbeA a1;
  ProbeA a2;
  const RID r0 = owner.MakeRid(&a0);
  const RID r1 = owner.MakeRid(&a1);
  const RID r2 = owner.MakeRid(&a2);

  EXPECT_FALSE(r0.IsNull());
  EXPECT_NE(r0.GetId(), r1.GetId());
  EXPECT_NE(r1.GetId(), r2.GetId());

  EXPECT_EQ(owner.GetOrNull(r0), &a0);
  EXPECT_EQ(owner.GetOrNull(r1), &a1);
  EXPECT_EQ(owner.GetOrNull(r2), &a2);
}

TEST(RidOwnerTest, FreeInvalidatesOnlyThatRid) {
  RIDOwner<ProbeA> owner;

  ProbeA a0;
  ProbeA a1;
  ProbeA a2;
  const RID r0 = owner.MakeRid(&a0);
  const RID r1 = owner.MakeRid(&a1);
  const RID r2 = owner.MakeRid(&a2);

  ASSERT_TRUE(owner.Free(r1));

  // 被释放的 RID 必须彻底失效
  EXPECT_EQ(owner.GetOrNull(r1), nullptr);
  EXPECT_FALSE(owner.Owns(r1));
  // 邻居必须毫发无损（off-by-one 的直接受害者）
  EXPECT_EQ(owner.GetOrNull(r0), &a0);
  EXPECT_EQ(owner.GetOrNull(r2), &a2);
  EXPECT_TRUE(owner.Owns(r0));
  EXPECT_TRUE(owner.Owns(r2));
}

TEST(RidOwnerTest, ReusedSlotYieldsValidRidAndKeepsNeighbours) {
  RIDOwner<ProbeA> owner;

  ProbeA a0;
  ProbeA a1;
  ProbeA a2;
  const RID r0 = owner.MakeRid(&a0);
  const RID r1 = owner.MakeRid(&a1);
  const RID r2 = owner.MakeRid(&a2);
  ASSERT_TRUE(owner.Free(r1));

  // 复用刚释放的槽：新 RID 必须立即可查，且不得改写邻居槽
  ProbeA reused;
  const RID r3 = owner.MakeRid(&reused);
  EXPECT_EQ(owner.GetOrNull(r3), &reused);
  EXPECT_TRUE(owner.Owns(r3));
  EXPECT_EQ(owner.GetOrNull(r0), &a0);
  EXPECT_EQ(owner.GetOrNull(r2), &a2);

  // 旧 RID 不得因为槽被复用而"复活"（世代校验兜底）
  EXPECT_EQ(owner.GetOrNull(r1), nullptr);
  EXPECT_FALSE(owner.Owns(r1));
}

TEST(RidOwnerTest, RepeatedAllocFreeCycleStaysConsistent) {
  // 反复复用同一槽：任何一次让旧 RID 命中新对象、或让常驻对象丢失，都算失败
  RIDOwner<ProbeA> owner;
  ProbeA keep;
  const RID keep_rid = owner.MakeRid(&keep);

  LocalVector<RID> issued;
  LocalVector<ProbeA> pool;  // 稳定地址的替身对象池（禁裸 new/delete）
  pool.Resize(64);
  for (int i = 0; i < 64; ++i) {
    pool[i].value = i;
    const RID rid = owner.MakeRid(&pool[i]);
    issued.PushBack(rid);
    EXPECT_EQ(owner.GetOrNull(rid), &pool[i]);
    EXPECT_EQ(owner.GetOrNull(keep_rid), &keep);
    ASSERT_TRUE(owner.Free(rid));
    EXPECT_EQ(owner.GetOrNull(rid), nullptr);
  }

  // 所有历史 RID 都必须永久失效
  for (uint32 i = 0; i < issued.Size(); ++i) {
    EXPECT_EQ(owner.GetOrNull(issued[i]), nullptr);
  }
  EXPECT_EQ(owner.GetOrNull(keep_rid), &keep);
}

// ═══════════════════════════════════════════════════════════════════════
// 跨表唯一性（FreeDriverResourceByRid 的逐表 Owns 探测依赖此性质）
// ═══════════════════════════════════════════════════════════════════════

TEST(RidOwnerTest, DifferentOwnersNeverShareRidValues) {
  RIDOwner<ProbeA> owner_a;
  RIDOwner<ProbeB> owner_b;

  ProbeA a0;
  ProbeA a1;
  ProbeB b0;
  ProbeB b1;
  const RID ra0 = owner_a.MakeRid(&a0);
  const RID ra1 = owner_a.MakeRid(&a1);
  const RID rb0 = owner_b.MakeRid(&b0);
  const RID rb1 = owner_b.MakeRid(&b1);

  // 数值必须互不相同，否则"按 RID 反查类型"的级联释放会误杀
  EXPECT_NE(ra0.GetId(), rb0.GetId());
  EXPECT_NE(ra1.GetId(), rb1.GetId());
  EXPECT_NE(ra0.GetId(), rb1.GetId());
  EXPECT_NE(ra1.GetId(), rb0.GetId());

  // 且 A 表的 RID 绝不能被 B 表认下（反之亦然）
  EXPECT_FALSE(owner_b.Owns(ra0));
  EXPECT_FALSE(owner_b.Owns(ra1));
  EXPECT_FALSE(owner_a.Owns(rb0));
  EXPECT_FALSE(owner_a.Owns(rb1));
  EXPECT_EQ(owner_b.GetOrNull(ra0), nullptr);
  EXPECT_EQ(owner_a.GetOrNull(rb0), nullptr);
}

TEST(RidOwnerTest, CrossTableUniquenessSurvivesFreeAndReuse) {
  // 两表交错分配/释放后仍不得出现同值 RID
  RIDOwner<ProbeA> owner_a;
  RIDOwner<ProbeB> owner_b;
  LocalVector<uint64> seen;

  LocalVector<ProbeA> pool_a;
  LocalVector<ProbeB> pool_b;
  pool_a.Resize(16);
  pool_b.Resize(16);
  for (int i = 0; i < 16; ++i) {
    const RID ra = owner_a.MakeRid(&pool_a[i]);
    const RID rb = owner_b.MakeRid(&pool_b[i]);
    seen.PushBack(ra.GetId());
    seen.PushBack(rb.GetId());
    ASSERT_TRUE(owner_a.Free(ra));
    ASSERT_TRUE(owner_b.Free(rb));
  }

  const uint64 dup = FirstDuplicate(seen);
  EXPECT_EQ(dup, 0u) << "RIDOwner 之间出现了同值 RID 0x" << std::hex << dup
                    << "，级联释放会跨表误杀资源";
}

TEST(RidOwnerTest, ThreadSafeVariantKeepsSameSemantics) {
  // kThreadSafe = true 的表（RD 的 buffer/texture/framebuffer/render_pass/
  // pipeline/uniform_set）必须与非线程安全变体行为一致
  RIDOwner<ProbeA, true> owner_a;
  RIDOwner<ProbeB, true> owner_b;

  ProbeA a;
  ProbeB b;
  const RID ra = owner_a.MakeRid(&a);
  const RID rb = owner_b.MakeRid(&b);

  EXPECT_NE(ra.GetId(), rb.GetId());
  EXPECT_FALSE(owner_b.Owns(ra));
  EXPECT_EQ(owner_a.GetOrNull(rb), nullptr);
  EXPECT_TRUE(owner_a.Free(ra));
  EXPECT_FALSE(owner_a.Owns(ra));
}

TEST(RidOwnerTest, ForEachReplaysIssuedRids) {
  // 级联释放与 Shutdown 都靠 ForEach 拿 RID——它必须回放出当初发出的值
  RIDOwner<ProbeA> owner;
  ProbeA a0;
  ProbeA a1;
  const RID r0 = owner.MakeRid(&a0);
  owner.Free(r0);           // 让 a1 落在"复用槽"上，覆盖两种来源
  const RID r1 = owner.MakeRid(&a1);

  LocalVector<RID> seen;
  owner.ForEach([&seen](RID p_rid, void* p_obj) {
    seen.PushBack(p_rid);
    (void)p_obj;
  });

  ASSERT_EQ(seen.Size(), 1u);
  EXPECT_EQ(seen[0].GetId(), r1.GetId());
  EXPECT_EQ(owner.GetOrNull(seen[0]), &a1);
}

}  // namespace
}  // namespace arhud
