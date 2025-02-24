/*!
 * Copyright 2021-2022 by XGBoost Contributors
 */
#ifndef XGBOOST_TREE_HIST_EVALUATE_SPLITS_H_
#define XGBOOST_TREE_HIST_EVALUATE_SPLITS_H_

#include <algorithm>
#include <memory>
#include <numeric>
#include <limits>
#include <utility>
#include <vector>

#include "xgboost/task.h"
#include "../param.h"
#include "../constraints.h"
#include "../split_evaluator.h"
#include "../../common/categorical.h"
#include "../../common/random.h"
#include "../../common/hist_util.h"
#include "../../data/gradient_index.h"

namespace xgboost {
namespace tree {

template <typename GradientSumT, typename ExpandEntry> class HistEvaluator {
 private:
  struct NodeEntry {
    /*! \brief statics for node entry */
    GradStats stats;
    /*! \brief loss of this node, without split */
    bst_float root_gain{0.0f};
  };

 private:
  TrainParam param_;
  std::shared_ptr<common::ColumnSampler> column_sampler_;
  TreeEvaluator tree_evaluator_;
  int32_t n_threads_ {0};
  FeatureInteractionConstraintHost interaction_constraints_;
  std::vector<NodeEntry> snode_;
  ObjInfo task_;

  // if sum of statistics for non-missing values in the node
  // is equal to sum of statistics for all values:
  // then - there are no missing values
  // else - there are missing values
  bool static SplitContainsMissingValues(const GradStats e, const NodeEntry &snode) {
    if (e.GetGrad() == snode.stats.GetGrad() &&
        e.GetHess() == snode.stats.GetHess()) {
      return false;
    } else {
      return true;
    }
  }

  // Enumerate/Scan the split values of specific feature
  // Returns the sum of gradients corresponding to the data points that contains
  // a non-missing value for the particular feature fid.
  template <int d_step, SplitType split_type>
  GradStats EnumerateSplit(common::HistogramCuts const &cut, common::Span<size_t const> sorted_idx,
                           const common::GHistRow<GradientSumT> &hist, bst_feature_t fidx,
                           bst_node_t nidx,
                           TreeEvaluator::SplitEvaluator<TrainParam> const &evaluator,
                           SplitEntry *p_best) const {
    static_assert(d_step == +1 || d_step == -1, "Invalid step.");

    // aliases
    const std::vector<uint32_t> &cut_ptr = cut.Ptrs();
    const std::vector<bst_float> &cut_val = cut.Values();
    auto const &parent = snode_[nidx];
    int32_t n_bins{static_cast<int32_t>(cut_ptr.at(fidx + 1) - cut_ptr[fidx])};
    auto f_hist = hist.subspan(cut_ptr[fidx], n_bins);

    // statistics on both sides of split
    GradStats left_sum;
    GradStats right_sum;
    // best split so far
    SplitEntry best;

    // bin boundaries
    CHECK_LE(cut_ptr[fidx], static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
    CHECK_LE(cut_ptr[fidx + 1], static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
    // imin: index (offset) of the minimum value for feature fid need this for backward
    //       enumeration
    const auto imin = static_cast<int32_t>(cut_ptr[fidx]);
    // ibegin, iend: smallest/largest cut points for feature fid use int to allow for
    // value -1
    int32_t ibegin, iend;
    if (d_step > 0) {
      ibegin = static_cast<int32_t>(cut_ptr[fidx]);
      iend = static_cast<int32_t>(cut_ptr.at(fidx + 1));
    } else {
      ibegin = static_cast<int32_t>(cut_ptr[fidx + 1]) - 1;
      iend = static_cast<int32_t>(cut_ptr[fidx]) - 1;
    }

    auto calc_bin_value = [&](auto i) {
      switch (split_type) {
        case kNum: {
          left_sum.Add(hist[i].GetGrad(), hist[i].GetHess());
          right_sum.SetSubstract(parent.stats, left_sum);
          break;
        }
        case kOneHot: {
          // not-chosen categories go to left
          right_sum = GradStats{hist[i]};
          left_sum.SetSubstract(parent.stats, right_sum);
          break;
        }
        case kPart: {
          auto j = d_step == 1 ? (i - ibegin) : (ibegin - i);
          right_sum.Add(f_hist[sorted_idx[j]].GetGrad(), f_hist[sorted_idx[j]].GetHess());
          left_sum.SetSubstract(parent.stats, right_sum);
          break;
        }
      }
    };

    int32_t best_thresh{-1};
    for (int32_t i = ibegin; i != iend; i += d_step) {
      // start working
      // try to find a split
      calc_bin_value(i);
      bool improved{false};
      if (left_sum.GetHess() >= param_.min_child_weight &&
          right_sum.GetHess() >= param_.min_child_weight) {
        bst_float loss_chg;
        bst_float split_pt;
        if (d_step > 0) {
          // forward enumeration: split at right bound of each bin
          loss_chg =
              static_cast<float>(evaluator.CalcSplitGain(param_, nidx, fidx, GradStats{left_sum},
                                                         GradStats{right_sum}) -
                                 parent.root_gain);
          split_pt = cut_val[i];  // not used for partition based
          improved = best.Update(loss_chg, fidx, split_pt, d_step == -1, split_type != kNum,
                                 left_sum, right_sum);
        } else {
          // backward enumeration: split at left bound of each bin
          loss_chg =
              static_cast<float>(evaluator.CalcSplitGain(param_, nidx, fidx, GradStats{right_sum},
                                                         GradStats{left_sum}) -
                                 parent.root_gain);
          switch (split_type) {
            case kNum: {
              if (i == imin) {
                split_pt = cut.MinValues()[fidx];
              } else {
                split_pt = cut_val[i - 1];
              }
              break;
            }
            case kOneHot: {
              split_pt = cut_val[i];
              break;
            }
            case kPart: {
              split_pt = cut_val[i];
              break;
            }
          }
          improved = best.Update(loss_chg, fidx, split_pt, d_step == -1, split_type != kNum,
                                 right_sum, left_sum);
        }
        if (improved) {
          best_thresh = i;
        }
      }
    }

    if (split_type == kPart && best_thresh != -1) {
      auto n = common::CatBitField::ComputeStorageSize(n_bins);
      best.cat_bits.resize(n, 0);
      common::CatBitField cat_bits{best.cat_bits};

      if (d_step == 1) {
        std::for_each(sorted_idx.begin(), sorted_idx.begin() + (best_thresh - ibegin + 1),
                      [&](size_t c) { cat_bits.Set(cut_val[c + ibegin]); });
      } else {
        std::for_each(sorted_idx.rbegin(), sorted_idx.rbegin() + (ibegin - best_thresh),
                      [&](size_t c) { cat_bits.Set(cut_val[c + cut_ptr[fidx]]); });
      }
    }
    p_best->Update(best);

    switch (split_type) {
      case kNum:
        // Normal, accumulated to left
        return left_sum;
      case kOneHot:
        // Doesn't matter, not accumulating.
        return {};
      case kPart:
        // Accumulated to right due to chosen cats go to right.
        return right_sum;
    }
    return left_sum;
  }

 public:
  void EvaluateSplits(const common::HistCollection<GradientSumT> &hist,
                      common::HistogramCuts const &cut,
                      common::Span<FeatureType const> feature_types,
                      const RegTree &tree,
                      std::vector<ExpandEntry> *p_entries) {
    auto& entries = *p_entries;
    // All nodes are on the same level, so we can store the shared ptr.
    std::vector<std::shared_ptr<HostDeviceVector<bst_feature_t>>> features(
        entries.size());
    for (size_t nidx_in_set = 0; nidx_in_set < entries.size(); ++nidx_in_set) {
      auto nidx = entries[nidx_in_set].nid;
      features[nidx_in_set] =
          column_sampler_->GetFeatureSet(tree.GetDepth(nidx));
    }
    CHECK(!features.empty());
    const size_t grain_size =
        std::max<size_t>(1, features.front()->Size() / n_threads_);
    common::BlockedSpace2d space(entries.size(), [&](size_t nidx_in_set) {
      return features[nidx_in_set]->Size();
    }, grain_size);

    std::vector<ExpandEntry> tloc_candidates(n_threads_ * entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
      for (decltype(n_threads_) j = 0; j < n_threads_; ++j) {
        tloc_candidates[i * n_threads_ + j] = entries[i];
      }
    }
    auto evaluator = tree_evaluator_.GetEvaluator();
    auto const& cut_ptrs = cut.Ptrs();

    common::ParallelFor2d(space, n_threads_, [&](size_t nidx_in_set, common::Range1d r) {
      auto tidx = omp_get_thread_num();
      auto entry = &tloc_candidates[n_threads_ * nidx_in_set + tidx];
      auto best = &entry->split;
      auto nidx = entry->nid;
      auto histogram = hist[nidx];
      auto features_set = features[nidx_in_set]->ConstHostSpan();
      for (auto fidx_in_set = r.begin(); fidx_in_set < r.end(); fidx_in_set++) {
        auto fidx = features_set[fidx_in_set];
        bool is_cat = common::IsCat(feature_types, fidx);
        if (!interaction_constraints_.Query(nidx, fidx)) {
          continue;
        }
        if (is_cat) {
          auto n_bins = cut_ptrs.at(fidx + 1) - cut_ptrs[fidx];
          if (common::UseOneHot(n_bins, param_.max_cat_to_onehot, task_)) {
            EnumerateSplit<+1, kOneHot>(cut, {}, histogram, fidx, nidx, evaluator, best);
            EnumerateSplit<-1, kOneHot>(cut, {}, histogram, fidx, nidx, evaluator, best);
          } else {
            std::vector<size_t> sorted_idx(n_bins);
            std::iota(sorted_idx.begin(), sorted_idx.end(), 0);
            auto feat_hist = histogram.subspan(cut_ptrs[fidx], n_bins);
            // Sort the histogram to get contiguous partitions.
            std::stable_sort(sorted_idx.begin(), sorted_idx.end(), [&](size_t l, size_t r) {
              auto ret = evaluator.CalcWeightCat(param_, feat_hist[l]) <
                         evaluator.CalcWeightCat(param_, feat_hist[r]);
              return ret;
            });
            EnumerateSplit<+1, kPart>(cut, sorted_idx, histogram, fidx, nidx, evaluator, best);
            EnumerateSplit<-1, kPart>(cut, sorted_idx, histogram, fidx, nidx, evaluator, best);
          }
        } else {
          auto grad_stats =
              EnumerateSplit<+1, kNum>(cut, {}, histogram, fidx, nidx, evaluator, best);
          if (SplitContainsMissingValues(grad_stats, snode_[nidx])) {
            EnumerateSplit<-1, kNum>(cut, {}, histogram, fidx, nidx, evaluator, best);
          }
        }
      }
    });

    for (unsigned nidx_in_set = 0; nidx_in_set < entries.size();
         ++nidx_in_set) {
      for (auto tidx = 0; tidx < n_threads_; ++tidx) {
        entries[nidx_in_set].split.Update(
            tloc_candidates[n_threads_ * nidx_in_set + tidx].split);
      }
    }
  }
  // Add splits to tree, handles all statistic
  void ApplyTreeSplit(ExpandEntry const& candidate, RegTree *p_tree) {
    auto evaluator = tree_evaluator_.GetEvaluator();
    RegTree &tree = *p_tree;

    GradStats parent_sum = candidate.split.left_sum;
    parent_sum.Add(candidate.split.right_sum);
    auto base_weight =
        evaluator.CalcWeight(candidate.nid, param_, GradStats{parent_sum});

    auto left_weight =
        evaluator.CalcWeight(candidate.nid, param_, GradStats{candidate.split.left_sum});
    auto right_weight =
        evaluator.CalcWeight(candidate.nid, param_, GradStats{candidate.split.right_sum});

    if (candidate.split.is_cat) {
      std::vector<uint32_t> split_cats;
      if (candidate.split.cat_bits.empty()) {
        if (common::InvalidCat(candidate.split.split_value)) {
          common::InvalidCategory();
        }
        auto cat = common::AsCat(candidate.split.split_value);
        split_cats.resize(LBitField32::ComputeStorageSize(std::max(cat + 1, 1)), 0);
        LBitField32 cat_bits;
        cat_bits = LBitField32(split_cats);
        cat_bits.Set(cat);
      } else {
        split_cats = candidate.split.cat_bits;
        common::CatBitField cat_bits{split_cats};
      }
      tree.ExpandCategorical(
          candidate.nid, candidate.split.SplitIndex(), split_cats, candidate.split.DefaultLeft(),
          base_weight, left_weight * param_.learning_rate, right_weight * param_.learning_rate,
          candidate.split.loss_chg, parent_sum.GetHess(), candidate.split.left_sum.GetHess(),
          candidate.split.right_sum.GetHess());
    } else {
      tree.ExpandNode(candidate.nid, candidate.split.SplitIndex(), candidate.split.split_value,
                      candidate.split.DefaultLeft(), base_weight,
                      left_weight * param_.learning_rate, right_weight * param_.learning_rate,
                      candidate.split.loss_chg, parent_sum.GetHess(),
                      candidate.split.left_sum.GetHess(), candidate.split.right_sum.GetHess());
    }

    // Set up child constraints
    auto left_child = tree[candidate.nid].LeftChild();
    auto right_child = tree[candidate.nid].RightChild();
    tree_evaluator_.AddSplit(candidate.nid, left_child, right_child,
                             tree[candidate.nid].SplitIndex(), left_weight,
                             right_weight);

    auto max_node = std::max(left_child, tree[candidate.nid].RightChild());
    max_node = std::max(candidate.nid, max_node);
    snode_.resize(tree.GetNodes().size());
    snode_.at(left_child).stats = candidate.split.left_sum;
    snode_.at(left_child).root_gain = evaluator.CalcGain(
        candidate.nid, param_, GradStats{candidate.split.left_sum});
    snode_.at(right_child).stats = candidate.split.right_sum;
    snode_.at(right_child).root_gain = evaluator.CalcGain(
        candidate.nid, param_, GradStats{candidate.split.right_sum});

    interaction_constraints_.Split(candidate.nid,
                                   tree[candidate.nid].SplitIndex(), left_child,
                                   right_child);
  }

  auto Evaluator() const { return tree_evaluator_.GetEvaluator(); }
  auto const& Stats() const { return snode_; }
  auto Task() const { return task_; }

  float InitRoot(GradStats const& root_sum) {
    snode_.resize(1);
    auto root_evaluator = tree_evaluator_.GetEvaluator();

    snode_[0].stats = GradStats{root_sum.GetGrad(), root_sum.GetHess()};
    snode_[0].root_gain = root_evaluator.CalcGain(RegTree::kRoot, param_,
                                                  GradStats{snode_[0].stats});
    auto weight = root_evaluator.CalcWeight(RegTree::kRoot, param_,
                                            GradStats{snode_[0].stats});
    return weight;
  }

 public:
  // The column sampler must be constructed by caller since we need to preserve the rng
  // for the entire training session.
  explicit HistEvaluator(TrainParam const &param, MetaInfo const &info, int32_t n_threads,
                         std::shared_ptr<common::ColumnSampler> sampler, ObjInfo task)
      : param_{param},
        column_sampler_{std::move(sampler)},
        tree_evaluator_{param, static_cast<bst_feature_t>(info.num_col_), GenericParameter::kCpuId},
        n_threads_{n_threads},
        task_{task} {
    interaction_constraints_.Configure(param, info.num_col_);
    column_sampler_->Init(info.num_col_, info.feature_weights.HostVector(), param_.colsample_bynode,
                          param_.colsample_bylevel, param_.colsample_bytree);
  }
};

/**
 * \brief CPU implementation of update prediction cache, which calculates the leaf value
 *        for the last tree and accumulates it to prediction vector.
 *
 * \param p_last_tree The last tree being updated by tree updater
 */
template <typename Partitioner, typename GradientSumT, typename ExpandEntry>
void UpdatePredictionCacheImpl(GenericParameter const *ctx, RegTree const *p_last_tree,
                               std::vector<Partitioner> const &partitioner,
                               HistEvaluator<GradientSumT, ExpandEntry> const &hist_evaluator,
                               TrainParam const &param, linalg::VectorView<float> out_preds) {
  CHECK_GT(out_preds.Size(), 0U);

  CHECK(p_last_tree);
  auto const &tree = *p_last_tree;
  auto const &snode = hist_evaluator.Stats();
  auto evaluator = hist_evaluator.Evaluator();
  CHECK_EQ(out_preds.DeviceIdx(), GenericParameter::kCpuId);
  size_t n_nodes = p_last_tree->GetNodes().size();
  for (auto &part : partitioner) {
    CHECK_EQ(part.Size(), n_nodes);
    common::BlockedSpace2d space(
        part.Size(), [&](size_t node) { return part[node].Size(); }, 1024);
    common::ParallelFor2d(space, ctx->Threads(), [&](size_t nidx, common::Range1d r) {
      if (!tree[nidx].IsDeleted() && tree[nidx].IsLeaf()) {
        auto const &rowset = part[nidx];
        auto const &stats = snode[nidx];
        auto leaf_value =
            evaluator.CalcWeight(nidx, param, GradStats{stats.stats}) * param.learning_rate;
        for (const size_t *it = rowset.begin + r.begin(); it < rowset.begin + r.end(); ++it) {
          out_preds(*it) += leaf_value;
        }
      }
    });
  }
}
}  // namespace tree
}  // namespace xgboost
#endif  // XGBOOST_TREE_HIST_EVALUATE_SPLITS_H_
