#pragma once

#include <map>
#include <array>
#include "bounded.h"
#include "bounded_value_container.h"
#include "options.h"
#include "leveldb/env.h"

namespace sagitrs {

enum DefaultCounterType : uint32_t {
  LeafCount = 0,
    // Construct: Set to 1 for leaf node, 0 for non-leaf node.
    // Update: Never change.
  PutCount,
    // Construct: Set to 0 for all node.
    // Update: increase when buffer element is increased.
  DelCount,
    // Construct: Set to 0 for all node.
    // Update: increase when buffer element is decreased.
  GetCount,
    // Construct: Set to 0 for all nodee.
    // Update: increase when buffer element is read.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           
  DefaultCounterTypeMax
};
enum StatisticsType { 
  TypeLatest,           // in a time slice.
  TypeRecent,           // in several time slices.
  TypeHistorical,       // all.
};

struct AbstractStatistics {
  virtual void Inc(uint32_t label, int size) = 0;
  virtual void Get(uint32_t label) = 0;
  virtual void Scale(double k) = 0;
  virtual void Superposition(const AbstractStatistics& target) = 0;
};  

struct Counter {
  typedef uint32_t TypeLabel;
  typedef int64_t TypeData;
 private:
  std::array<TypeData, DefaultCounterTypeMax> list_;
 public:
  TypeData operator[](TypeLabel k) const { return list_[k]; }
  virtual void Inc(TypeLabel label, int size) { list_[label] += size; }
  //virtual void Set(TypeLabel label, TypeData size) { list_[label] = size; }
  virtual void Scale(double k) { for (auto& element : list_) element *= k;  }
  virtual void Superposition(const Counter& target, bool decrease = false) {
    for (size_t i = 0; i < DefaultCounterTypeMax; ++i)
      list_[i] += target[i] * (decrease ? -1 : 1);
  }
  void GetStringLog(std::vector<std::string>& set) {
    set.push_back("PUT=" + std::to_string(list_[PutCount]));
    set.push_back("DEL=" + std::to_string(list_[DelCount]));
    set.push_back("GET=" + std::to_string(list_[GetCount]));
    set.push_back("LEAF=" + std::to_string(list_[LeafCount]));
  }
};

struct HistoricalStatistics {
  typedef std::shared_ptr<Counter> CPTR;
  typedef uint64_t TypeTime;
  std::shared_ptr<StatisticsOptions> options_;
 private:
  std::map<TypeTime, CPTR> history_;
  CPTR recent_, akasha_;
  uint64_t Now() { return options_->NowTimeSlice(); }
  uint64_t LatestTimeSlice() const { return history_.rbegin()->first; }
  uint64_t OldestTimeSlice() const { return history_.begin()->first; }
  CPTR Current() {
    TypeTime now = Now();
    if (history_.empty() || now > LatestTimeSlice()) {
      CPTR tmp = std::make_shared<Counter>();
      history_[now] = tmp;
      CleanUpSnapshot();
    }
    return history_.rbegin()->second;
  }
  void PopCounter() {
    auto target = history_.begin()->second;
    akasha_->Superposition(*target, true);
    history_.erase(history_.begin());
  }
  void CleanUpSnapshot() {
    TypeTime now = Now();
    while (!history_.empty()) {
      if (OldestTimeSlice() + options_->TimeSliceMaximumSize() < now)
        PopCounter();
      else
        break;
    }
  }
 public:
  HistoricalStatistics(std::shared_ptr<StatisticsOptions> options) 
  : options_(options), 
    history_(),
    recent_(std::make_shared<Counter>()),
    akasha_(std::make_shared<Counter>()) {}
  HistoricalStatistics(const HistoricalStatistics& stats) = delete;

  CPTR at(StatisticsType type) {
    switch(type) {
    case TypeLatest: return Current(); 
    case TypeRecent: return recent_;
    case TypeHistorical: return akasha_;
    default: assert(false); return nullptr;
    }
  }
  void Inc(Counter::TypeLabel label, int size) { 
    Current()->Inc(label, size);
    akasha_->Inc(label, size);
    recent_->Inc(label, size); 
  }
  Counter::TypeData Get(StatisticsType type, Counter::TypeLabel label) { return (*at(type))[label]; }

  // When a node inherits the contents of the original node in proportion k, 
  // ParaTable must give an initialization scheme.
  virtual void Scale(double k) {
    for (auto& kv : history_) kv.second->Scale(k);
    akasha_->Scale(k);
    recent_->Scale(k);
  }
  // When a node merges another node, ParaTable must give a merging scheme.
  virtual void Superposition(const HistoricalStatistics& bro) {
    for (auto& kv : bro.history_) {
      auto p = history_.find(kv.first);
      const auto& counter = *kv.second;
      if (p == history_.end()) 
        history_[kv.first] = std::make_shared<Counter>(counter);
      else {
        p->second->Superposition(counter);
        recent_->Superposition(counter);
      }
    }
    akasha_->Superposition(*bro.akasha_);
    CleanUpSnapshot();
  }
  void GetStringLog(std::vector<std::string>& set) { at(TypeHistorical)->GetStringLog(set); }
};

struct Statistics {
  struct RangedHistoricalStatistics : public HistoricalStatistics {
    std::string guard_;
    RangedHistoricalStatistics(const Slice& guard, std::shared_ptr<StatisticsOptions> options)
    : HistoricalStatistics(options), guard_(guard.ToString()) {}
    int Compare(const Slice& key) const { return Slice(guard_).compare(key); }
    Slice Guard() const { return guard_; }
  };
  typedef std::shared_ptr<RangedHistoricalStatistics> RHSP;
  std::shared_ptr<StatisticsOptions> options_;
  std::vector<RHSP> shards_;
  uint64_t last_seperated_time_;
  Statistics() = delete;
  Statistics(const Statistics&) = delete;
  Statistics(std::shared_ptr<StatisticsOptions> options)
  : options_(options), 
    shards_({std::make_shared<RangedHistoricalStatistics>("", options)}),
    last_seperated_time_(0) {}
  virtual void Inc(const Slice& key, uint32_t label, int size) { at(key)->Inc(label, size); }
  virtual void Set(const Slice& key, uint32_t label, uint64_t size) { at(key)->Inc(label, size); }
  virtual uint64_t Get(StatisticsType type, Counter::TypeLabel label) const { return shards_[0]->Get(type, label); }
  virtual void Scale(double k) { for (auto& rhsp : shards_) rhsp->Scale(k); }
  virtual void Superposition(const Statistics& bro) { 
    for (auto rhsp : bro.shards_) InsertRHSP(rhsp);
    last_seperated_time_ = options_->NowTimeSlice();
  }
  void SetGuard(const Slice& key) { shards_[0]->guard_ = key.ToString(); }
  void Inherit(std::shared_ptr<Statistics> src, const Slice& guard) {
    for (size_t i = 0; i < src->shards_.size(); ++i) {
      auto shard = src->shards_[i];
      if (shard->Compare(guard) > 0) {
        assert(i > 0);
        if (i > 1) i--;
        for (size_t j = i; j < src->shards_.size(); ++j)
          InsertRHSP(src->shards_[j]);
        src->shards_.erase(src->shards_.begin() + i, src->shards_.end());
        break;
      }
    }
  }
  void GetStringLog(std::vector<std::string>& set) {
    for (RHSP bc : shards_)
      bc->GetStringLog(set);
  }
 private:
  RHSP at(const Slice& key) {
    assert(!shards_.empty());
    MergeCheck();
    for (auto iter = shards_.rbegin(); iter != shards_.rend(); ++iter) 
      if ((*iter)->Compare(key) <= 0)
         return *iter;
    assert(false);
    return nullptr;
  }
  // When a node merges another node, ParaTable must give a merging scheme.
  void InsertRHSP(RHSP stats) {
    for (size_t i = 0; i < shards_.size(); ++i) {
      if (shards_[i]->Compare(stats->Guard()) > 0) {
        shards_.insert(shards_.begin() + i, stats);
        return;
      }
    }
    shards_.push_back(stats);
  }
  void MergeCheck() {
    if (shards_.size() <= 1) return;
    uint64_t now = options_->NowTimeSlice();
    if (now < last_seperated_time_ + options_->TimeBeforeMerge())
      return;
    RHSP base = shards_[0];
    for (auto iter = shards_.begin()+1; iter != shards_.end(); iter++) {
      base->Superposition(**iter);
    }
    shards_.resize(1);
  }

};

}

/*

可统计信息：
1. 实时统计一颗树内的叶子结点：
TreeLeafCount = 1 (Leaf Create)
              | 0 + sum(child.LeafCount) (Inner Node) (Recalculate when merging/spliting)
TreeLeafCount += 1 (for inserting path)
               |-1 (for  deleting path)

2. 实时统计节点内读动作：
BufferReadCount = 0 (for all node)
BufferReadCount+= 1 (for readed node)
BufferReadCount=> 0 (destructing)

3. 实时统计一个节点内

3. 合并时特殊动作：暂存，当一个节点被另一个节点合并时，其统计信息暂存在该节点中，其代为记录该节点的统计情况
超过一定时间后合并。
分裂时：从原节点中继承所有暂存区间，跨区间统计信息会 

*/