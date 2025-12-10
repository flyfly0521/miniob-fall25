/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "common/sys/rc.h"
#include "sql/operator/physical_operator.h"
#include "sql/parser/parse.h"
#include "common/value.h"
#include "sql/expr/tuple_cell.h"
#include <functional>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

/**
 * @brief Hash Join 算子
 * @ingroup PhysicalOperator
 */
struct HashKeyNode {
  std::vector<Value> keys;  // 存储多个连接列的值
  
  bool operator==(const HashKeyNode& other) const {
    if (keys.size() != other.keys.size()) {
      return false;
    }
    for (size_t i = 0; i < keys.size(); i++) {
      if (keys[i].compare(other.keys[i]) != 0) {
        return false;
      }
    }
    return true;
  }
};

struct HashKeyNodeHasher {
  std::size_t operator()(const HashKeyNode& node) const {
    std::size_t hash = 0;
    for (const auto& key : node.keys) {
      // 使用简单的哈希组合方式
      std::size_t key_hash = 0;
      switch (key.attr_type()) {
        case AttrType::INTS:
          key_hash = std::hash<int>{}(key.get_int());
          break;
        case AttrType::FLOATS:
          key_hash = std::hash<float>{}(key.get_float());
          break;
        case AttrType::CHARS: {
          std::string str = key.get_string();
          key_hash = std::hash<std::string>{}(str);
          break;
        }
        case AttrType::DATES:
          key_hash = std::hash<int>{}(key.get_int());
          break;
        case AttrType::BOOLEANS:
          key_hash = std::hash<bool>{}(key.get_boolean());
          break;
        default:
          key_hash = 0;
          break;
      }
      hash = hash ^ (key_hash << 1);
    }
    return hash;
  }
};

class HashJoinPhysicalOperator : public PhysicalOperator
{
public:
  HashJoinPhysicalOperator() = default;
  virtual ~HashJoinPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::HASH_JOIN; }

  OpType get_op_type() const override { return OpType::INNERNLJOIN; }

  void set_predicate(unique_ptr<Expression>&& predicate) {
    ASSERT(predicate->type() == ExprType::CONJUNCTION, "predicate should be a conjunction expression");
    join_conditions_ = std::move(predicate);
  }

  virtual double calculate_cost(
      LogicalProperty *prop, const vector<LogicalProperty *> &child_log_props, CostModel *cm) override
  {
    return 0.0;
  }

  RC     open(Trx *trx) override;
  RC     next() override;
  RC     close() override;
  Tuple *current_tuple() override;

private:
  RC left_next();   //! 左表遍历下一条数据
  RC right_next();  //! 右表遍历下一条数据，如果上一轮结束了就重新开始新的一轮
  RC get_hashkey_specs();  //! 从连接条件中提取哈希键的 TupleCellSpec
  RC extract_hash_keys(Tuple* tuple, HashKeyNode& node, const std::vector<TupleCellSpec>& key_specs);

private:
  Trx *trx_ = nullptr;
  bool right_emited_{false};

  //! 左表右表的真实对象是在PhysicalOperator::children_中，这里是为了写的时候更简单
  PhysicalOperator *left_        = nullptr;
  PhysicalOperator *right_       = nullptr;
  Tuple            *left_tuple_  = nullptr;
  Tuple            *right_tuple_ = nullptr;
  JoinedTuple       joined_tuple_;         //! 当前关联的左右两个tuple
  
  std::vector<TupleCellSpec> key_specs_;  // 存储等值条件的字段 TupleCellSpec（每对字段存两个）
  unique_ptr<Expression> join_conditions_; // 连接谓词表达式
  std::vector<Tuple*> left_tuples_; // 存储左表所有的tuple指针
  // 哈希表：key -> tuple索引列表
  // 注意：当多个left tuple有相同的连接键值时，它们会被存储在同一个vector中
  // std::unordered_map内部已经处理了哈希冲突（不同的key hash到同一个bucket）
  using hashed_map_t = std::unordered_map<HashKeyNode, vector<size_t>, HashKeyNodeHasher>;
  hashed_map_t hash_table_; // 哈希表，key -> tuple索引列表（支持一个key对应多个tuple）
  
  // Probe 阶段的辅助变量
  size_t current_probe_tuple_index_ = 0;  // 当前 probe 的右表 tuple 索引
  std::vector<size_t> current_matched_indices_;  // 当前右表 tuple 匹配的左表 tuple 索引列表
  size_t current_match_index_ = 0;  // 当前正在输出的匹配索引
};