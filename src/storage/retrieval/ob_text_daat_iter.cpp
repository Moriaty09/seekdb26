/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_NOT_NULL(param.base_param_->relevance_expr_)
             && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  active_heads_ = 0;
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  active_heads_ = 0;
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::fill_merge_heap()
{
  // Text DaaT owns concrete token iterators (as reset/reuse also require).
  // Keep large-term merging on the common path and preserve its heap policy.
  if (nullptr == simple_merge_heap_) {
    return ObSRDaaTIterImpl::fill_merge_heap();
  }
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
    const int64_t iter_idx = next_round_iter_idxes_[i];
    auto *iter = static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(iter_idx));
    ObSRMergeItem &item = small_heads_[iter_idx];
    item.iter_idx_ = iter_idx;
    if (OB_ISNULL(iter)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(iter->next_posting(iter_domain_ids_[iter_idx], item.relevance_))) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    } else {
      if (OB_NOT_NULL(iter_param_->dim_weights_)) {
        item.relevance_ = item.relevance_ * iter_param_->field_boost_ * iter_param_->dim_weights_->at(iter_idx);
      }
      active_heads_ |= (uint64_t(1) << iter_idx);
    }
  }
  if (OB_SUCC(ret)) {
    if (0 == active_heads_) {
      ret = OB_ITER_END;
    } else {
      next_round_cnt_ = 0;
    }
  }
  return ret;
}

int ObTextDaaTIter::collect_dims_by_id(
    const ObDatum *&id_datum, double &relevance, bool &got_valid_id)
{
  if (nullptr == simple_merge_heap_) {
    return ObSRDaaTIterImpl::collect_dims_by_id(id_datum, relevance, got_valid_id);
  }
  static_assert(ObSRSimpleMerger::USE_SIMPLE_MERGER_MAX_TABLE_CNT <= 64,
                "active posting heads must fit in the mask");
  int ret = OB_SUCCESS;
  int64_t winner = -1;
  uint64_t matched_heads = 0;
  relevance = 0.0;
  got_valid_id = false;
  // At most three live heads: select their minimum without sorting and
  // moving heap entries after every posting. Unconsumed heads remain valid
  // until their own token iterator is advanced in the next fill operation.
  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    const uint64_t bit = uint64_t(1) << i;
    if (0 == (active_heads_ & bit)) {
    } else if (winner < 0) {
      winner = i;
      matched_heads = bit;
    } else {
      int64_t cmp = 0;
      if (OB_FAIL(merge_cmp_.cmp(small_heads_[i], small_heads_[winner], cmp))) {
      } else if (cmp < 0) {
        winner = i;
        matched_heads = bit;
      } else if (0 == cmp) {
        matched_heads |= bit;
      }
    }
  }
  if (OB_SUCC(ret) && winner < 0) {
    ret = OB_ERR_UNEXPECTED;
  }
  // Preserve the heap's ascending iterator-index order for equal IDs,
  // including floating-point score accumulation and the chosen ID datum.
  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    const uint64_t bit = uint64_t(1) << i;
    if (0 != (matched_heads & bit)) {
      if (OB_FAIL(relevance_collector_->collect_one_dim(i, small_heads_[i].relevance_))) {
      } else {
        id_datum = iter_domain_ids_[i];
        next_round_iter_idxes_[next_round_cnt_++] = i;
        active_heads_ &= ~bit;
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(id_datum)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(relevance_collector_->get_result(relevance, got_valid_id))) {
  } else if (got_valid_id && OB_FAIL(process_collected_row(*id_datum, relevance))) {
  }
  return ret;
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_NOT_NULL(iter_param_->relevance_expr_)) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
