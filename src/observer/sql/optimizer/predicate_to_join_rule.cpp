/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/optimizer/predicate_to_join_rule.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/predicate_logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"
#include "sql/expr/expression.h"
#include "sql/operator/join_logical_operator.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace std;

/**
 * @brief 递归遍历算子树，收集所有 TableGetLogicalOperator
 */
void PredicateToJoinRewriter::visitor(LogicalOperator *oper, std::vector<TableGetLogicalOperator *> &table_get_ops)
{
  if (oper == nullptr) {
    return;
  }
  
  if (oper->type() == LogicalOperatorType::TABLE_GET) {
    table_get_ops.push_back(static_cast<TableGetLogicalOperator *>(oper));
  }
  
  for (auto &child : oper->children()) {
    visitor(child.get(), table_get_ops);
  }
}

/**
 * @brief 从表达式中收集涉及的表名
 */
static void collect_table_names(Expression *expr, unordered_set<string> &table_names)
{
  if (expr == nullptr) {
    return;
  }
  
  if (expr->type() == ExprType::FIELD) {
    FieldExpr *field_expr = static_cast<FieldExpr *>(expr);
    table_names.insert(field_expr->table_name());
  } else if (expr->type() == ExprType::COMPARISON) {
    ComparisonExpr *cmp_expr = static_cast<ComparisonExpr *>(expr);
    collect_table_names(cmp_expr->left().get(), table_names);
    collect_table_names(cmp_expr->right().get(), table_names);
  } else if (expr->type() == ExprType::CONJUNCTION) {
    ConjunctionExpr *conj_expr = static_cast<ConjunctionExpr *>(expr);
    for (auto &child : conj_expr->children()) {
      collect_table_names(child.get(), table_names);
    }
  }
}

/**
 * @brief 收集子树中所有表名
 */
static void collect_subtree_table_names(LogicalOperator *oper, unordered_set<string> &table_names)
{
  if (oper == nullptr) {
    return;
  }
  
  if (oper->type() == LogicalOperatorType::TABLE_GET) {
    TableGetLogicalOperator *table_get = static_cast<TableGetLogicalOperator *>(oper);
    table_names.insert(table_get->table()->name());
  }
  
  for (auto &child : oper->children()) {
    collect_subtree_table_names(child.get(), table_names);
  }
}

/**
 * @brief 检查表达式中的所有表是否都在给定的表集合中
 */
static bool expr_tables_contained_in(Expression *expr, const unordered_set<string> &available_tables)
{
  unordered_set<string> expr_tables;
  collect_table_names(expr, expr_tables);
  
  for (const auto &table : expr_tables) {
    if (available_tables.find(table) == available_tables.end()) {
      return false;
    }
  }
  return true;
}

/**
 * @brief 递归地将谓词下推到 Join 或 TableGet 算子
 * @param oper 当前算子
 * @param predicates 待下推的谓词列表
 * @param pushed 是否成功下推了至少一个谓词
 */
static void push_predicates_down(
    LogicalOperator *oper,
    vector<unique_ptr<Expression>> &predicates,
    bool &pushed)
{
  if (oper == nullptr || predicates.empty()) {
    return;
  }
  
  // 收集当前子树中的所有表名
  unordered_set<string> subtree_tables;
  collect_subtree_table_names(oper, subtree_tables);
  
  if (oper->type() == LogicalOperatorType::TABLE_GET) {
    // 处理 TableGet 算子
    TableGetLogicalOperator *table_get = static_cast<TableGetLogicalOperator *>(oper);
    string table_name = table_get->table()->name();
    
    vector<unique_ptr<Expression>> pushdown_exprs;
    
    for (auto it = predicates.begin(); it != predicates.end();) {
      unordered_set<string> expr_tables;
      collect_table_names(it->get(), expr_tables);
      
      // 如果谓词只涉及当前表，下推到 TableGet
      if (expr_tables.size() == 1 && expr_tables.count(table_name) > 0) {
        pushdown_exprs.push_back(std::move(*it));
        it = predicates.erase(it);
        pushed = true;
      } else {
        ++it;
      }
    }
    
    if (!pushdown_exprs.empty()) {
      // 合并已有的谓词
      auto &existing_predicates = table_get->predicates();
      for (auto &expr : pushdown_exprs) {
        existing_predicates.push_back(std::move(expr));
      }
    }
  } else if (oper->type() == LogicalOperatorType::JOIN) {
    // 处理 Join 算子
    JoinLogicalOperator *join_oper = static_cast<JoinLogicalOperator *>(oper);
    
    // 先尝试下推到子节点
    for (auto &child : oper->children()) {
      push_predicates_down(child.get(), predicates, pushed);
    }
    
    // 剩余的谓词，如果涉及的表都在当前 Join 的子树中，则下推到 Join
    for (auto it = predicates.begin(); it != predicates.end();) {
      if (expr_tables_contained_in(it->get(), subtree_tables)) {
        join_oper->add_join_predicate(std::move(*it));
        it = predicates.erase(it);
        pushed = true;
      } else {
        ++it;
      }
    }
  } else {
    // 其他类型的算子，递归处理子节点
    for (auto &child : oper->children()) {
      push_predicates_down(child.get(), predicates, pushed);
    }
  }
}

/**
 * @brief 从 ConjunctionExpr 中提取所有子表达式
 */
static void extract_predicates(unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &predicates)
{
  if (expr == nullptr) {
    return;
  }
  
  if (expr->type() == ExprType::CONJUNCTION) {
    ConjunctionExpr *conj_expr = static_cast<ConjunctionExpr *>(expr.get());
    if (conj_expr->conjunction_type() == ConjunctionExpr::Type::AND) {
      for (auto &child : conj_expr->children()) {
        extract_predicates(child, predicates);
      }
      conj_expr->children().clear();
    } else {
      // OR 表达式不拆分，整体处理
      predicates.push_back(std::move(expr));
    }
  } else {
    predicates.push_back(std::move(expr));
  }
}

RC PredicateToJoinRewriter::rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made)
{
  if (oper == nullptr || oper->type() != LogicalOperatorType::PREDICATE) {
    return RC::SUCCESS;
  }
  
  // 检查是否有子节点
  if (oper->children().empty()) {
    return RC::SUCCESS;
  }
  
  // 获取谓词表达式
  vector<unique_ptr<Expression>> &exprs = oper->expressions();
  if (exprs.empty()) {
    return RC::SUCCESS;
  }
  
  // 提取所有谓词
  vector<unique_ptr<Expression>> predicates;
  for (auto &expr : exprs) {
    extract_predicates(expr, predicates);
  }
  
  if (predicates.empty()) {
    return RC::SUCCESS;
  }
  
  // 尝试将谓词下推到子节点
  bool pushed = false;
  for (auto &child : oper->children()) {
    push_predicates_down(child.get(), predicates, pushed);
  }
  
  if (pushed) {
    change_made = true;
  }
  
  // 将剩余的谓词放回 PredicateLogicalOperator
  exprs.clear();
  if (!predicates.empty()) {
    if (predicates.size() == 1) {
      exprs.push_back(std::move(predicates[0]));
    } else {
      // 多个谓词用 AND 连接
      unique_ptr<ConjunctionExpr> conj_expr(new ConjunctionExpr(ConjunctionExpr::Type::AND, predicates));
      exprs.push_back(std::move(conj_expr));
    }
  } else {
    // 所有谓词都下推了，放一个 true 占位
    Value value((bool)true);
    exprs.push_back(unique_ptr<Expression>(new ValueExpr(value)));
  }
  
  return RC::SUCCESS;
}
