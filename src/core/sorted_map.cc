// Copyright 2023, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "core/sorted_map.h"

#include <cmath>

extern "C" {
#include "redis/listpack.h"
#include "redis/redis_aux.h"
#include "redis/util.h"
#include "redis/zmalloc.h"
}

#include "base/endian.h"
#include "base/flags.h"
#include "base/logging.h"

using namespace std;

ABSL_FLAG(bool, use_zset_tree, false, "If true use b+tree for zset implementation");

namespace dfly {
namespace detail {

namespace {

// We tag sds pointers to allow customizable comparison function that supports both
// Lex and Numeric comparison. It's safe to do on linux systems because its memory address range
// is within 56 bit space.
constexpr uint64_t kInfTag = 1ULL << 63;
constexpr uint64_t kIgnoreDoubleTag = 1ULL << 62;
constexpr uint64_t kSdsMask = (1ULL << 60) - 1;

// Approximated dictionary size.
size_t DictMallocSize(dict* d) {
  size_t res = zmalloc_usable_size(d->ht_table[0]) + zmalloc_usable_size(d->ht_table[1]) +
               znallocx(sizeof(dict));

  return res + dictSize(d) * 16;  // approximation.
}

unsigned char* zzlInsertAt(unsigned char* zl, unsigned char* eptr, sds ele, double score) {
  unsigned char* sptr;
  char scorebuf[128];
  int scorelen;

  scorelen = d2string(scorebuf, sizeof(scorebuf), score);
  if (eptr == NULL) {
    zl = lpAppend(zl, (unsigned char*)ele, sdslen(ele));
    zl = lpAppend(zl, (unsigned char*)scorebuf, scorelen);
  } else {
    /* Insert member before the element 'eptr'. */
    zl = lpInsertString(zl, (unsigned char*)ele, sdslen(ele), eptr, LP_BEFORE, &sptr);

    /* Insert score after the member. */
    zl = lpInsertString(zl, (unsigned char*)scorebuf, scorelen, sptr, LP_AFTER, NULL);
  }
  return zl;
}

inline zskiplistNode* Next(bool reverse, zskiplistNode* ln) {
  return reverse ? ln->backward : ln->level[0].forward;
}

inline bool IsUnder(bool reverse, double score, const zrangespec& spec) {
  return reverse ? zslValueGteMin(score, &spec) : zslValueLteMax(score, &spec);
}

double GetObjScore(const void* obj) {
  sds s = (sds)obj;
  char* ptr = s + sdslen(s) + 1;
  return absl::bit_cast<double>(absl::little_endian::Load64(ptr));
}

void SetObjScore(void* obj, double score) {
  sds s = (sds)obj;
  char* ptr = s + sdslen(s) + 1;
  absl::little_endian::Store64(ptr, absl::bit_cast<uint64_t>(score));
}

// buf must be at least 10 chars long.
// Builds a tagged key that can be used for querying open/closed bounds.
void* BuilScoredKey(double score, bool is_str_inf, char buf[]) {
  buf[0] = SDS_TYPE_5;  // length 0.
  buf[1] = 0;
  absl::little_endian::Store64(buf + 2, absl::bit_cast<uint64_t>(score));
  void* key = buf + 1;

  // to include/exclude the score we set the secondary string to +inf.
  // +inf causes exclusion for minimum bound and causes inclusion for maximum bound.
  if (is_str_inf) {
    key = (void*)(uint64_t(key) | kInfTag);
  }
  return key;
}

}  // namespace

void SortedMap::RdImpl::Init() {
  dict = dictCreate(&zsetDictType);
  zsl = zslCreate();
}

size_t SortedMap::RdImpl::MallocSize() const {
  return DictMallocSize(dict) + zmalloc_size(zsl);
}

bool SortedMap::RdImpl::Insert(double score, sds member) {
  zskiplistNode* znode = zslInsert(zsl, score, member);
  int ret = dictAdd(dict, member, &znode->score);
  return ret == DICT_OK;
}

int SortedMap::RdImpl::Add(double score, sds ele, int in_flags, int* out_flags, double* newscore) {
  zskiplistNode* znode;

  /* Turn options into simple to check vars. */
  const bool incr = (in_flags & ZADD_IN_INCR) != 0;
  const bool nx = (in_flags & ZADD_IN_NX) != 0;
  const bool xx = (in_flags & ZADD_IN_XX) != 0;
  const bool gt = (in_flags & ZADD_IN_GT) != 0;
  const bool lt = (in_flags & ZADD_IN_LT) != 0;

  *out_flags = 0; /* We'll return our response flags. */
  double curscore;

  dictEntry* de = dictFind(dict, ele);
  if (de != NULL) {
    /* NX? Return, same element already exists. */
    if (nx) {
      *out_flags |= ZADD_OUT_NOP;
      return 1;
    }

    curscore = *(double*)dictGetVal(de);

    /* Prepare the score for the increment if needed. */
    if (incr) {
      score += curscore;
      if (isnan(score)) {
        *out_flags |= ZADD_OUT_NAN;
        return 0;
      }
    }

    /* GT/LT? Only update if score is greater/less than current. */
    if ((lt && score >= curscore) || (gt && score <= curscore)) {
      *out_flags |= ZADD_OUT_NOP;
      return 1;
    }

    *newscore = score;

    /* Remove and re-insert when score changes. */
    if (score != curscore) {
      znode = zslUpdateScore(zsl, curscore, ele, score);
      /* Note that we did not removed the original element from
       * the hash table representing the sorted set, so we just
       * update the score. */
      dictGetVal(de) = &znode->score; /* Update score ptr. */
      *out_flags |= ZADD_OUT_UPDATED;
    }
    return 1;
  } else if (!xx) {
    ele = sdsdup(ele);
    znode = zslInsert(zsl, score, ele);
    CHECK_EQ(DICT_OK, dictAdd(dict, ele, &znode->score));

    *out_flags |= ZADD_OUT_ADDED;
    *newscore = score;
    return 1;
  }

  *out_flags |= ZADD_OUT_NOP;
  return 1;
}

optional<unsigned> SortedMap::RdImpl::GetRank(sds ele, bool reverse) const {
  dictEntry* de = dictFind(dict, ele);
  if (de == NULL)
    return std::nullopt;

  double score = *(double*)dictGetVal(de);
  unsigned rank = zslGetRank(zsl, score, ele);

  /* Existing elements always have a rank. */
  DCHECK(rank != 0);
  return reverse ? zsl->length - rank : rank - 1;
}

optional<double> SortedMap::RdImpl::GetScore(sds member) const {
  dictEntry* de = dictFind(dict, member);
  if (de == NULL)
    return std::nullopt;

  return *(double*)dictGetVal(de);
}

SortedMap::ScoredArray SortedMap::RdImpl::GetRange(const zrangespec& range, unsigned offset,
                                                   unsigned limit, bool reverse) const {
  /* If reversed, get the last node in range as starting point. */
  zskiplistNode* ln;

  if (reverse) {
    ln = zslLastInRange(zsl, &range);
  } else {
    ln = zslFirstInRange(zsl, &range);
  }

  /* If there is an offset, just traverse the number of elements without
   * checking the score because that is done in the next loop. */
  while (ln && offset--) {
    ln = Next(reverse, ln);
  }

  ScoredArray result;
  while (ln && limit--) {
    /* Abort when the node is no longer in range. */
    if (!IsUnder(reverse, ln->score, range))
      break;

    result.emplace_back(string{ln->ele, sdslen(ln->ele)}, ln->score);

    /* Move to next node */
    ln = Next(reverse, ln);
  }
  return result;
}

SortedMap::ScoredArray SortedMap::RdImpl::GetLexRange(const zlexrangespec& range, unsigned offset,
                                                      unsigned limit, bool reverse) const {
  zskiplistNode* ln;
  /* If reversed, get the last node in range as starting point. */
  if (reverse) {
    ln = zslLastInLexRange(zsl, &range);
  } else {
    ln = zslFirstInLexRange(zsl, &range);
  }

  /* If there is an offset, just traverse the number of elements without
   * checking the score because that is done in the next loop. */
  while (ln && offset--) {
    ln = Next(reverse, ln);
  }

  ScoredArray result;
  while (ln && limit--) {
    /* Abort when the node is no longer in range. */
    if (reverse) {
      if (!zslLexValueGteMin(ln->ele, &range))
        break;
    } else {
      if (!zslLexValueLteMax(ln->ele, &range))
        break;
    }

    result.emplace_back(string{ln->ele, sdslen(ln->ele)}, ln->score);

    /* Move to next node */
    ln = Next(reverse, ln);
  }

  return result;
}

uint8_t* SortedMap::RdImpl::ToListPack() const {
  uint8_t* lp = lpNew(0);

  /* Approach similar to zslFree(), since we want to free the skiplist at
   * the same time as creating the listpack. */
  zskiplistNode* node = zsl->header->level[0].forward;

  while (node) {
    lp = zzlInsertAt(lp, NULL, node->ele, node->score);
    node = node->level[0].forward;
  }

  return lp;
}

bool SortedMap::RdImpl::Delete(sds member) {
  dictEntry* de = dictUnlink(dict, member);
  if (de == NULL)
    return false;

  /* Get the score in order to delete from the skiplist later. */
  double score = *(double*)dictGetVal(de);

  /* Delete from the hash table and later from the skiplist.
   * Note that the order is important: deleting from the skiplist
   * actually releases the SDS string representing the element,
   * which is shared between the skiplist and the hash table, so
   * we need to delete from the skiplist as the final step. */
  dictFreeUnlinkedEntry(dict, de);
  if (htNeedsResize(dict))
    dictResize(dict);

  /* Delete from skiplist. */
  int retval = zslDelete(zsl, score, member, NULL);
  DCHECK(retval);

  return true;
}

SortedMap::ScoredArray SortedMap::RdImpl::PopTopScores(unsigned count, bool reverse) {
  zskiplistNode* ln;
  if (reverse) {
    ln = zsl->tail;
  } else {
    ln = zsl->header->level[0].forward;
  }

  ScoredArray result;
  while (ln && count--) {
    result.emplace_back(string{ln->ele, sdslen(ln->ele)}, ln->score);

    /* we can delete the element now */
    CHECK(Delete(ln->ele));

    ln = Next(reverse, ln);
  }
  return result;
}

size_t SortedMap::RdImpl::Count(const zrangespec& range) const {
  /* Find first element in range */
  zskiplistNode* zn = zslFirstInRange(zsl, &range);

  /* Use rank of first element, if any, to determine preliminary count */
  if (zn == NULL)
    return 0;

  unsigned long rank = zslGetRank(zsl, zn->score, zn->ele);
  size_t count = (zsl->length - (rank - 1));

  /* Find last element in range */
  zn = zslLastInRange(zsl, &range);

  /* Use rank of last element, if any, to determine the actual count */
  if (zn != NULL) {
    rank = zslGetRank(zsl, zn->score, zn->ele);
    count -= (zsl->length - rank);
  }

  return count;
}

size_t SortedMap::RdImpl::LexCount(const zlexrangespec& range) const {
  /* Find first element in range */
  zskiplistNode* zn = zslFirstInLexRange(zsl, &range);

  /* Use rank of first element, if any, to determine preliminary count */
  if (zn == NULL)
    return 0;

  unsigned long rank = zslGetRank(zsl, zn->score, zn->ele);
  size_t count = (zsl->length - (rank - 1));

  /* Find last element in range */
  zn = zslLastInLexRange(zsl, &range);

  /* Use rank of last element, if any, to determine the actual count */
  if (zn != NULL) {
    rank = zslGetRank(zsl, zn->score, zn->ele);
    count -= (zsl->length - rank);
  }
  return count;
}

bool SortedMap::RdImpl::Iterate(unsigned start_rank, unsigned len, bool reverse,
                                absl::FunctionRef<bool(sds, double)> cb) const {
  zskiplistNode* ln;

  /* Check if starting point is trivial, before doing log(N) lookup. */
  if (reverse) {
    ln = zsl->tail;
    unsigned long llen = zsl->length;
    if (start_rank > 0)
      ln = zslGetElementByRank(zsl, llen - start_rank);
  } else {
    ln = zsl->header->level[0].forward;
    if (start_rank > 0)
      ln = zslGetElementByRank(zsl, start_rank + 1);
  }

  bool success = true;
  while (success && len--) {
    DCHECK(ln != NULL);
    success = cb(ln->ele, ln->score);
    ln = reverse ? ln->backward : ln->level[0].forward;
    if (!ln)
      break;
  }
  return success;
}

uint64_t SortedMap::RdImpl::Scan(uint64_t cursor,
                                 absl::FunctionRef<void(std::string_view, double)> cb) const {
  auto scanCb = [](void* privdata, const dictEntry* de) {
    auto* cb = reinterpret_cast<absl::FunctionRef<void(std::string_view, double)>*>(privdata);

    sds key = (sds)de->key;
    double score = *reinterpret_cast<double*>(dictGetVal(de));
    (*cb)(std::string_view(key, sdslen(key)), score);
  };

  return dictScan(this->dict, cursor, scanCb, NULL, &cb);
}

int SortedMap::DfImpl::ScoreSdsPolicy::KeyCompareTo::operator()(ScoreSds a, ScoreSds b) const {
  sds sdsa = (sds)(uint64_t(a) & kSdsMask);
  sds sdsb = (sds)(uint64_t(b) & kSdsMask);

  // if omit score comparison if at least one of the elements is tagged to ignore the score.
  // These tags exist only when passing keys for query methods, tree elements are never tagged.
  if ((uint64_t(a) & kIgnoreDoubleTag) == 0 && (uint64_t(b) & kIgnoreDoubleTag) == 0) {
    double sa = GetObjScore(sdsa);
    double sb = GetObjScore(sdsb);

    if (sa < sb)
      return -1;
    if (sa > sb)
      return 1;
  }

  // Marks +inf.
  if (uint64_t(a) & kInfTag)
    return 1;

  if (uint64_t(b) & kInfTag)
    return -1;

  return sdscmp(sdsa, sdsb);
}

int SortedMap::DfImpl::Add(double score, sds ele, int in_flags, int* out_flags, double* newscore) {
  // does not take ownership over ele.
  DCHECK(!isnan(score));

  // TODO: to introduce AddOrFind in score_map.
  ScoreSds obj = score_map->FindObj(ele);

  if (obj == nullptr) {
    // Adding a new element.
    if (in_flags & ZADD_IN_XX) {
      *out_flags = ZADD_OUT_NOP;
      return 1;
    }

    auto [newk, added] = score_map->AddOrUpdate(string_view{ele, sdslen(ele)}, score);
    DCHECK(added);

    *out_flags = ZADD_OUT_ADDED;
    *newscore = score;
    obj = newk;
    added = score_tree->Insert(obj);
    DCHECK(added);

    return 1;
  }

  // Updating an existing element.
  if ((in_flags & ZADD_IN_NX)) {
    // Updating an existing element.
    *out_flags = ZADD_OUT_NOP;
    return 1;
  }

  if (in_flags & ZADD_IN_INCR) {
    score += GetObjScore(obj);
    if (isnan(score)) {
      *out_flags = ZADD_OUT_NAN;
      return 0;
    }
  }

  // Update the score.
  CHECK(score_tree->Delete(obj));
  SetObjScore(obj, score);
  CHECK(score_tree->Insert(obj));
  *out_flags = ZADD_OUT_UPDATED;
  *newscore = score;
  return 1;
}

optional<double> SortedMap::DfImpl::GetScore(sds ele) const {
  ScoreSds obj = score_map->FindObj(ele);
  if (obj != nullptr) {
    return GetObjScore(obj);
  }

  return std::nullopt;
}

void SortedMap::DfImpl::Init(PMR_NS::memory_resource* mr) {
  score_map = new ScoreMap(mr);
  score_tree = new ScoreTree(mr);
}

void SortedMap::DfImpl::Free() {
  DVLOG(1) << "Freeing SortedMap";
  delete score_tree;
  delete score_map;
}

// Takes ownership over ele.
bool SortedMap::DfImpl::Insert(double score, sds ele) {
  DVLOG(1) << "Inserting " << ele << " with score " << score;

  auto [newk, added] = score_map->AddOrUpdate(string_view{ele, sdslen(ele)}, score);
  DCHECK(added);

  added = score_tree->Insert(newk);
  DCHECK(added);
  sdsfree(ele);

  return true;
}

optional<unsigned> SortedMap::DfImpl::GetRank(sds ele, bool reverse) const {
  ScoreSds obj = score_map->FindObj(ele);
  if (obj == nullptr)
    return std::nullopt;

  optional rank = score_tree->GetRank(obj);
  DCHECK(rank);
  return reverse ? score_map->Size() - *rank - 1 : *rank;
}

SortedMap::ScoredArray SortedMap::DfImpl::GetRange(const zrangespec& range, unsigned offset,
                                                   unsigned limit, bool reverse) const {
  LOG(FATAL) << "TBD";
  return {};
}

SortedMap::ScoredArray SortedMap::DfImpl::GetLexRange(const zlexrangespec& range, unsigned offset,
                                                      unsigned limit, bool reverse) const {
  LOG(FATAL) << "TBD";
  return {};
}

uint8_t* SortedMap::DfImpl::ToListPack() const {
  LOG(FATAL) << "TBD";
  return nullptr;
}

bool SortedMap::DfImpl::Delete(sds ele) {
  ScoreSds obj = score_map->FindObj(ele);
  if (obj == nullptr)
    return false;

  CHECK(score_tree->Delete(obj));
  CHECK(score_map->Erase(ele));
  return true;
}

size_t SortedMap::DfImpl::MallocSize() const {
  // TODO: add malloc used to BPTree.
  return score_map->SetMallocUsed() + score_map->ObjMallocUsed() + score_tree->Size() * 256;
}

bool SortedMap::DfImpl::Reserve(size_t sz) {
  score_map->Reserve(sz);
  return true;
}

size_t SortedMap::DfImpl::DeleteRangeByRank(unsigned start, unsigned end) {
  LOG(FATAL) << "TBD";
  return 0;
}

size_t SortedMap::DfImpl::DeleteRangeByScore(const zrangespec& range) {
  LOG(FATAL) << "TBD";
  return 0;
}

size_t SortedMap::DfImpl::DeleteRangeByLex(const zlexrangespec& range) {
  LOG(FATAL) << "TBD";
  return 0;
}

SortedMap::ScoredArray SortedMap::DfImpl::PopTopScores(unsigned count, bool reverse) {
  DCHECK_EQ(score_map->Size(), score_tree->Size());
  size_t sz = score_map->Size();

  ScoredArray res;

  if (sz == 0)
    return res;

  if (count >= sz)
    count = score_map->Size();

  res.reserve(count);
  unsigned rank = 0;
  unsigned step = 0;

  if (reverse) {
    rank = sz - 1;
    step = 1;
  }

  for (unsigned i = 0; i < count; ++i) {
    score_tree->DeleteRangeByRank(rank, rank, [&](ScoreSds obj) {
      res.emplace_back(string{(sds)obj, sdslen((sds)obj)}, GetObjScore(obj));
    });
    score_map->Erase(res.back().first);
    rank -= step;
  }

  return res;
}

size_t SortedMap::DfImpl::Count(const zrangespec& range) const {
  DCHECK_LE(range.min, range.max);

  if (score_tree->Size() == 0)
    return 0;

  // build min key.
  char buf[16];

  ScoreSds range_key = BuilScoredKey(range.min, range.minex, buf);
  auto path = score_tree->GEQ(range_key);
  if (path.Empty())
    return 0;

  ScoreSds bound = path.Terminal();

  if (range.minex) {
    DCHECK_GT(GetObjScore(bound), range.min);
  } else {
    DCHECK_GE(GetObjScore(bound), range.min);
  }

  uint32_t min_rank = path.Rank();

  // Now build the max key.
  // If we need to exclude the maximum score, set the key'sstring part to empty string,
  // otherwise set it to infinity.
  range_key = BuilScoredKey(range.max, !range.maxex, buf);

  path = score_tree->GEQ(range_key);
  if (path.Empty()) {
    return score_tree->Size() - min_rank;
  }

  bound = path.Terminal();
  uint32_t max_rank = path.Rank();
  if (range.maxex || GetObjScore(bound) > range.max) {
    if (max_rank <= min_rank)
      return 0;
    --max_rank;
  }

  // max_rank could be less than min_rank, for example, if the range is [a, a).
  return max_rank < min_rank ? 0 : max_rank - min_rank + 1;
}

size_t SortedMap::DfImpl::LexCount(const zlexrangespec& range) const {
  LOG(FATAL) << "TBD";
  return 0;
}

bool SortedMap::DfImpl::Iterate(unsigned start_rank, unsigned len, bool reverse,
                                absl::FunctionRef<bool(sds, double)> cb) const {
  DCHECK_GT(len, 0u);
  unsigned end_rank = start_rank + len - 1;
  bool success;
  if (reverse) {
    success = score_tree->IterateReverse(
        start_rank, end_rank, [&](ScoreSds obj) { return cb((sds)obj, GetObjScore(obj)); });
  } else {
    success = score_tree->Iterate(start_rank, end_rank,
                                  [&](ScoreSds obj) { return cb((sds)obj, GetObjScore(obj)); });
  }

  return success;
}

uint64_t SortedMap::DfImpl::Scan(uint64_t cursor,
                                 absl::FunctionRef<void(std::string_view, double)> cb) const {
  auto scan_cb = [&cb](const void* obj) {
    sds ele = (sds)obj;
    cb(string_view{ele, sdslen(ele)}, GetObjScore(obj));
  };

  return this->score_map->Scan(cursor, std::move(scan_cb));
}

/***************************************************************************/
/* SortedMap */
/***************************************************************************/
SortedMap::SortedMap(PMR_NS::memory_resource* mr) : impl_(RdImpl()), mr_res_(mr) {
  if (absl::GetFlag(FLAGS_use_zset_tree)) {
    impl_ = DfImpl();
  }
  std::visit(Overload{[](RdImpl& impl) { impl.Init(); }, [mr](DfImpl& impl) { impl.Init(mr); }},
             impl_);
}

SortedMap::~SortedMap() {
  std::visit([](auto& impl) { impl.Free(); }, impl_);
}

// taken from zsetConvert
unique_ptr<SortedMap> SortedMap::FromListPack(PMR_NS::memory_resource* res, const uint8_t* lp) {
  uint8_t* zl = (uint8_t*)lp;
  unsigned char *eptr, *sptr;
  unsigned char* vstr;
  unsigned int vlen;
  long long vlong;
  sds ele;

  unique_ptr<SortedMap> zs(new SortedMap(res));

  eptr = lpSeek(zl, 0);
  if (eptr != NULL) {
    sptr = lpNext(zl, eptr);
    CHECK(sptr != NULL);
  }

  while (eptr != NULL) {
    double score = zzlGetScore(sptr);
    vstr = lpGetValue(eptr, &vlen, &vlong);
    if (vstr == NULL)
      ele = sdsfromlonglong(vlong);
    else
      ele = sdsnewlen((char*)vstr, vlen);

    CHECK(zs->Insert(score, ele));
    zzlNext(zl, &eptr, &sptr);
  }

  return zs;
}

}  // namespace detail
}  // namespace dfly
