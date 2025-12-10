/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/hash_join_physical_operator.h"
#include "common/sys/rc.h"
#include "sql/expr/expression.h"
#include <unordered_map>

RC HashJoinPhysicalOperator::open(Trx *trx) {
  RC rc{RC::SUCCESS};
  if (children_.size() != 2) {
    LOG_WARN("Hash join operator should have 2 children");
    return RC::INTERNAL;
  }
  
  // 初始化成员变量
  left_ = children_[0].get();
  right_ = children_[1].get();
  trx_ = trx;
  right_emited_ = false;
  left_tuple_ = nullptr;
  right_tuple_ = nullptr;
  key_specs_.clear();
  hash_table_.clear();
  current_probe_tuple_index_ = 0;
  current_matched_indices_.clear();
  current_match_index_ = 0;
  
  // 从连接条件中提取哈希键的 TupleCellSpec
  rc = get_hashkey_specs();
  if (rc != RC::SUCCESS) {
    return rc;
  }
  
  // 打开左算子（build side）
  rc = left_->open(trx);
  if (rc != RC::SUCCESS) {
    return rc;
  }
  
  // 构建哈希表
  while ((rc = left_->next()) == RC::SUCCESS) {
    left_tuple_ = left_->current_tuple();
    
    HashKeyNode key_node;
    rc = extract_hash_keys(left_tuple_, key_node, key_specs_);
    if (rc != RC::SUCCESS) {
      left_->close();
      return rc;
    }
    
    // 复制并存储 tuple
    Tuple* tuple_copy = nullptr;
    rc = left_tuple_->copy(tuple_copy);
    if (rc != RC::SUCCESS) {
      left_->close();
      return rc;
    }
    left_tuples_.push_back(tuple_copy);
    
    // 将 tuple 索引存入哈希表（支持一个 key 对应多个 tuple）
    size_t tuple_index = left_tuples_.size() - 1;
    hash_table_[key_node].push_back(tuple_index);
  }
  
  if (rc != RC::RECORD_EOF) {
    left_->close();
    return rc;
  }
  
  // 关闭左算子
  rc = left_->close();
  if (rc != RC::SUCCESS) {
    return rc;
  }
  
  // 打开右算子（probe side）
  rc = right_->open(trx);
  if (rc != RC::SUCCESS) {
    return rc;
  }
  
  return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::next() {
  RC rc {RC::SUCCESS};
  
  // 如果当前右表 tuple 还有未输出的匹配，继续输出
  if (current_match_index_ < current_matched_indices_.size()) {
    size_t left_tuple_idx = current_matched_indices_[current_match_index_];
    left_tuple_ = left_tuples_[left_tuple_idx];
    joined_tuple_.set_left(left_tuple_);
    joined_tuple_.set_right(right_tuple_);
    current_match_index_++;
    return RC::SUCCESS;
  }
  
  // 读取下一个右表 tuple
  while (true) {
    rc = right_->next();
    if (rc != RC::SUCCESS) {
      if (rc == RC::RECORD_EOF) {
        return RC::RECORD_EOF;
      }
      return rc;
    }
    
    right_tuple_ = right_->current_tuple();
    
    // 从右表 tuple 中提取哈希键
    HashKeyNode probe_key;
    rc = extract_hash_keys(right_tuple_, probe_key, key_specs_);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    
    // 在哈希表中查找匹配
    auto it = hash_table_.find(probe_key);
    if (it != hash_table_.end() && !it->second.empty()) {
      // 找到匹配，设置当前匹配列表
      current_matched_indices_ = it->second;
      current_match_index_ = 0;
      
      // 输出第一个匹配
      size_t left_tuple_idx = current_matched_indices_[current_match_index_];
      left_tuple_ = left_tuples_[left_tuple_idx];
      joined_tuple_.set_left(left_tuple_);
      joined_tuple_.set_right(right_tuple_);
      current_match_index_++;
      return RC::SUCCESS;
    }
    // 如果没有匹配，继续读取下一个右表 tuple
  }
  
  return RC::RECORD_EOF;
}

RC HashJoinPhysicalOperator::close() {
  RC rc = RC::SUCCESS;
  
  // 清理左表 tuple
  for (auto & ptr : left_tuples_) {
    if (ptr != nullptr) {
      delete ptr;
      ptr = nullptr;
    }
  }
  left_tuples_.clear();
  
  // 关闭右算子
  if (right_ != nullptr) {
    rc = right_->close();
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to close right oper. rc=%s", strrc(rc));
    }
  }
  
  return rc;
}

Tuple * HashJoinPhysicalOperator::current_tuple() {
  return &joined_tuple_;
}

RC HashJoinPhysicalOperator::get_hashkey_specs() {
  if (join_conditions_.get() == nullptr) {
    return RC::SUCCESS;
  }
  
  key_specs_.clear();
  
  ConjunctionExpr* conj_expr = dynamic_cast<ConjunctionExpr*>(join_conditions_.get());
  if (conj_expr == nullptr) {
    return RC::INTERNAL;
  }
  
  // 遍历所有连接条件，只提取等值条件的字段
  for (auto& child : conj_expr->children()) {
    ComparisonExpr* cmp_expr = dynamic_cast<ComparisonExpr*>(child.get());
    if (cmp_expr == nullptr || !cmp_expr->field_field_comparison()) {
      continue;  // 只处理字段-字段比较
    }
    
    // 只处理等值比较（非等值条件不能用于哈希键）
    if (cmp_expr->comp() != CompOp::EQUAL_TO) {
      continue;
    }
    
    // 获取左右两个字段表达式
    Expression* left_expr = cmp_expr->left().get();
    Expression* right_expr = cmp_expr->right().get();
    
    FieldExpr* left_field = nullptr;
    FieldExpr* right_field = nullptr;
    
    if (left_expr->type() == ExprType::FIELD) {
      left_field = dynamic_cast<FieldExpr*>(left_expr);
    }
    if (right_expr->type() == ExprType::FIELD) {
      right_field = dynamic_cast<FieldExpr*>(right_expr);
    }
    
    if (left_field == nullptr || right_field == nullptr) {
      continue;
    }
    
    // 存储等值条件的左右字段 TupleCellSpec
    // extract_hash_keys 会尝试找到属于当前 tuple 的那个字段
    TupleCellSpec spec_left(left_field->table_name(), left_field->field_name());
    TupleCellSpec spec_right(right_field->table_name(), right_field->field_name());
    
    // 两个 spec 都存储（作为一对）
    key_specs_.push_back(spec_left);
    key_specs_.push_back(spec_right);
  }
  
  return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::extract_hash_keys(Tuple* tuple, HashKeyNode& node, const std::vector<TupleCellSpec>& key_specs) {
  RC rc{RC::SUCCESS};
  node.keys.clear();
  
  // 每两个 spec 是一对（来自同一个等值条件的左右字段）
  // 我们需要找到属于这个 tuple 的那个字段
  for (size_t i = 0; i < key_specs.size(); i += 2) {
    Value value;
    // 尝试第一个 spec
    rc = tuple->find_cell(key_specs[i], value);
    if (rc == RC::SUCCESS) {
      node.keys.push_back(value);
      continue;
    }
    
    // 尝试第二个 spec
    if (i + 1 < key_specs.size()) {
      rc = tuple->find_cell(key_specs[i + 1], value);
      if (rc == RC::SUCCESS) {
        node.keys.push_back(value);
        continue;
      }
    }
    
    // 如果都找不到，返回错误（但这不应该发生，因为连接条件应该引用有效的字段）
    // 注意：对于多表连接，可能某些字段不在当前 tuple 中，这是正常的
    // 我们跳过找不到的字段
  }
  
  return RC::SUCCESS;
}
