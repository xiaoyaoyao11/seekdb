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

#define USING_LOG_PREFIX SHARE

#include "observer/change_stream/ob_cs_plugin_ai_column.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_sql_string.h"
#include "share/ob_server_struct.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_schema_runtime_service.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_schema_struct.h"
#include "share/tablet/ob_tablet_mapping_operator.h"
#include "storage/blocksstable/ob_datum_row.h"
#include "storage/blocksstable/ob_row_reader.h"
#include "storage/blocksstable/ob_storage_datum.h"
#include "storage/memtable/mvcc/ob_row_data.h"
#include "share/schema/ob_col_desc.h"
#include "common/mysqlclient/ob_mysql_transaction.h"

namespace oceanbase
{
using namespace common;
namespace share
{

ObCSAiColumnProcessor::ObCSAiColumnProcessor(ObCSExecCtx &ctx)
  : ctx_(ctx)
{
}

ObCSAiColumnProcessor::~ObCSAiColumnProcessor()
{
}

int ObCSAiColumnProcessor::init_schema_guard_()
{
  int ret = common::OB_SUCCESS;
  ObMultiVersionSchemaService *schema_service =
      nullptr != ::oceanbase::share::server_service<::oceanbase::share::schema::ObSchemaRuntimeService>()
          ? ::oceanbase::share::server_service<::oceanbase::share::schema::ObSchemaRuntimeService>()->get_schema_service()
          : nullptr;
  if (OB_ISNULL(schema_service)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_service->get_runtime_schema_guard(
      schema_guard_, ctx_.schema_version_,
      ObMultiVersionSchemaService::RefreshSchemaMode::FORCE_FALLBACK))) {
  }
  return ret;
}

int ObCSAiColumnProcessor::resolve_table_id_from_tablet_id_(
    const common::ObTabletID &tablet_id,
    uint64_t &table_id)
{
  int ret = common::OB_SUCCESS;
  table_id = common::OB_INVALID_ID;
  if (OB_UNLIKELY(!tablet_id.is_valid())) {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (tablet_id.is_inner_tablet()) {
    table_id = tablet_id.id();
  } else if (OB_SUCC(tablet_to_table_.get_refactored(tablet_id.id(), table_id))) {
    if (common::OB_INVALID_ID == table_id) {
      ret = common::OB_TABLET_NOT_EXIST;
    }
  } else if (OB_HASH_NOT_EXIST != ret) {
  } else if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else {
    common::ObSEArray<common::ObTabletID, 1> tablet_ids;
    common::ObSEArray<ObTabletTablePair, 1> infos;
    if (OB_FAIL(tablet_ids.push_back(tablet_id))) {
    } else if (OB_FAIL(ObTabletMappingTableOperator::batch_get(
                   *GCTX.sql_proxy_, tablet_ids, infos))) {
      if (common::OB_ITEM_NOT_MATCH == ret) {
        ret = common::OB_TABLET_NOT_EXIST;
        table_id = common::OB_INVALID_ID;
        if (OB_FAIL(tablet_to_table_.set_refactored(tablet_id.id(), common::OB_INVALID_ID))) {
        }
      }
    } else if (OB_UNLIKELY(infos.count() != 1)) {
      ret = common::OB_ERR_UNEXPECTED;
    } else {
      table_id = infos.at(0).get_table_id();
      if (common::OB_INVALID_ID == table_id) {
        ret = common::OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(tablet_to_table_.set_refactored(tablet_id.id(), table_id))) {
      }
    }
  }
  return ret;
}

// Evaluate every AI column of the table through the SQL engine and publish the
// results. The statement is executed in its own transaction with the inner SQL
// connection (whose session is marked as an inner session, so it may write the
// otherwise read-only AI columns).
int ObCSAiColumnProcessor::handle_table_(uint64_t table_id)
{
  int ret = common::OB_SUCCESS;
  const schema::ObTableSchema *table_schema = NULL;
  const schema::ObDatabaseSchema *db_schema = NULL;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_guard_.get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema) || !table_schema->is_user_table()
             || !table_schema->has_ai_column()) {
    // nothing to do
  } else if (OB_FAIL(schema_guard_.get_database_schema(table_schema->get_database_id(),
                                                       db_schema))) {
  } else if (OB_ISNULL(db_schema)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else {
    const common::ObString &db_name = db_schema->get_database_name_str();
    const common::ObString &table_name = table_schema->get_table_name_str();
    for (schema::ObTableSchema::const_column_iterator it = table_schema->column_begin();
         OB_SUCC(ret) && NULL != it && it != table_schema->column_end(); ++it) {
      const schema::ObColumnSchemaV2 *column = *it;
      if (OB_ISNULL(column) || !column->is_ai_column()
          || column->get_ai_column_expr().empty()) {
        continue;
      }
      const common::ObString &expr = column->get_ai_column_expr();
      const common::ObString &col_name = column->get_column_name_str();
      ObSqlString sql;
      if (OB_FAIL(sql.append_fmt(
              "UPDATE `%.*s`.`%.*s` SET `%.*s` = (%.*s) "
              "WHERE `%.*s` IS NULL AND (%.*s) IS NOT NULL LIMIT 8",
              db_name.length(), db_name.ptr(),
              table_name.length(), table_name.ptr(),
              col_name.length(), col_name.ptr(),
              expr.length(), expr.ptr(),
              col_name.length(), col_name.ptr(),
              expr.length(), expr.ptr()))) {
      } else {
        ObMySQLTransaction trans;
        int tmp_ret = trans.start(GCTX.sql_proxy_);
        if (OB_SUCCESS != tmp_ret) {
          LOG_WARN("failed to start ai column transaction", K(tmp_ret), K(table_id));
        } else {
          int64_t affected_rows = 0;
          tmp_ret = trans.write(sql.ptr(), affected_rows);
          if (OB_SUCCESS != tmp_ret) {
            int end_ret = trans.end(false);
            LOG_WARN("failed to update ai column", K(tmp_ret), K(table_id), K(end_ret));
          } else {
            int end_ret = trans.end(true);
            if (OB_SUCCESS != end_ret) {
              LOG_WARN("failed to commit ai column result", K(end_ret), K(table_id));
            } else if (affected_rows > 0) {
              LOG_INFO("ai column generated", K(table_id), K(affected_rows));
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObCSAiColumnProcessor::build_pk_literal_(const schema::ObColumnSchemaV2 &column,
                                             const blocksstable::ObStorageDatum &datum,
                                             common::ObSqlString &condition)
{
  int ret = common::OB_SUCCESS;
  if (datum.is_null()) {
    ret = condition.append("NULL");
  } else if (column.get_meta_type().is_integer_type()) {
    ret = condition.append_fmt("%ld", datum.get_int());
  } else {
    const char *data = datum.ptr_;
    const int64_t len = datum.len_;
    if (OB_FAIL(condition.append("'"))) {
    } else if (OB_ISNULL(data)) {
      ret = common::OB_ERR_UNEXPECTED;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < len; ++i) {
        if ('\'' == data[i]) {
          ret = condition.append("''");
        } else {
          ret = condition.append(data + i, 1);
        }
      }
      if (OB_SUCC(ret)) {
        ret = condition.append("'");
      }
    }
  }
  return ret;
}

int ObCSAiColumnProcessor::build_pk_condition_(const schema::ObTableSchema &table,
                                               const common::ObIArray<schema::ObColDesc> &col_descs,
                                               const memtable::ObRowData &row_data,
                                               common::ObSqlString &condition)
{
  int ret = common::OB_SUCCESS;
  bool first = true;
  blocksstable::ObRowReader row_reader;
  if (OB_ISNULL(row_data.data_) || row_data.size_ <= 0) {
    ret = common::OB_INVALID_ARGUMENT;
  }
  for (int64_t j = 0; OB_SUCC(ret) && j < col_descs.count(); ++j) {
    const schema::ObColumnSchemaV2 *column = table.get_column_schema(col_descs.at(j).col_id_);
    if (OB_ISNULL(column) || !column->is_rowkey_column()) {
      continue;
    }
    blocksstable::ObStorageDatum datum;
    if (OB_FAIL(row_reader.read_column(row_data.data_, row_data.size_, j, datum))) {
    } else if (datum.is_nop()) {
      ret = common::OB_ERR_UNEXPECTED;
    } else if (!first && OB_FAIL(condition.append(" AND "))) {
    } else if (OB_FALSE_IT(first = false)) {
    } else if (OB_FAIL(condition.append_fmt("`%.*s` = ",
                                            column->get_column_name_str().length(),
                                            column->get_column_name_str().ptr()))) {
    } else if (OB_FAIL(build_pk_literal_(*column, datum, condition))) {
    }
  }
  return ret;
}

// A user UPDATE that touches a column referenced by an AI expression must
// regenerate that AI column for the updated row. The rowkey is read from the
// redo row, so the statement targets exactly one row.
int ObCSAiColumnProcessor::handle_update_row_(uint64_t table_id, const ObCSRow &row)
{
  int ret = common::OB_SUCCESS;
  const schema::ObTableSchema *table_schema = NULL;
  const schema::ObDatabaseSchema *db_schema = NULL;
  common::ObSEArray<schema::ObColDesc, 16> col_descs;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_guard_.get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema) || !table_schema->is_user_table()
             || !table_schema->has_ai_column()) {
    // nothing to do
  } else if (OB_FAIL(table_schema->get_store_column_ids(col_descs))) {
  } else if (OB_FAIL(schema_guard_.get_database_schema(table_schema->get_database_id(),
                                                       db_schema))) {
  } else if (OB_ISNULL(db_schema)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(row.new_row_.data_) || row.new_row_.size_ <= 0) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    // Collect the ordinary columns carried by this redo row. Our own write-back
    // only carries AI columns, so it is ignored here (this also breaks the loop).
    common::ObSEArray<common::ObString, 8> changed_columns;
    blocksstable::ObRowReader row_reader;
    bool decode_ok = true;
    for (int64_t j = 0; OB_SUCC(ret) && decode_ok && j < col_descs.count(); ++j) {
      const schema::ObColumnSchemaV2 *column = table_schema->get_column_schema(col_descs.at(j).col_id_);
      if (OB_ISNULL(column) || column->is_ai_column() || column->is_rowkey_column()) {
        continue;
      }
      blocksstable::ObStorageDatum datum;
      if (OB_FAIL(row_reader.read_column(row.new_row_.data_, row.new_row_.size_, j, datum))) {
        decode_ok = false;
      } else if (!datum.is_nop() && OB_FAIL(changed_columns.push_back(column->get_column_name_str()))) {
      }
    }
    if (OB_SUCC(ret) && changed_columns.count() > 0) {
      ObSqlString pk_condition;
      if (OB_FAIL(build_pk_condition_(*table_schema, col_descs, row.new_row_, pk_condition))) {
      } else {
        const common::ObString &db_name = db_schema->get_database_name_str();
        const common::ObString &table_name = table_schema->get_table_name_str();
        for (schema::ObTableSchema::const_column_iterator it = table_schema->column_begin();
             OB_SUCC(ret) && NULL != it && it != table_schema->column_end(); ++it) {
          const schema::ObColumnSchemaV2 *column = *it;
          if (OB_ISNULL(column) || !column->is_ai_column()
              || column->get_ai_column_expr().empty()) {
            continue;
          }
          const common::ObString &expr = column->get_ai_column_expr();
          bool touched = false;
          for (int64_t c = 0; !touched && c < changed_columns.count(); ++c) {
            const common::ObString &col_name = changed_columns.at(c);
            for (int64_t k = 0; !touched && k + col_name.length() <= expr.length(); ++k) {
              if (0 == strncasecmp(expr.ptr() + k, col_name.ptr(), col_name.length())) {
                touched = true;
              }
            }
          }
          if (!touched) {
            continue;
          }
          ObSqlString sql;
          if (OB_FAIL(sql.append_fmt(
                  "UPDATE `%.*s`.`%.*s` SET `%.*s` = (%.*s) WHERE %.*s",
                  db_name.length(), db_name.ptr(),
                  table_name.length(), table_name.ptr(),
                  column->get_column_name_str().length(), column->get_column_name_str().ptr(),
                  expr.length(), expr.ptr(),
                  pk_condition.length(), pk_condition.ptr()))) {
          } else {
            ObMySQLTransaction trans;
            int tmp_ret = trans.start(GCTX.sql_proxy_);
            if (OB_SUCCESS != tmp_ret) {
              LOG_WARN("failed to start ai column transaction", K(tmp_ret), K(table_id));
            } else {
              int64_t affected_rows = 0;
              tmp_ret = trans.write(sql.ptr(), affected_rows);
              if (OB_SUCCESS != tmp_ret) {
                int end_ret = trans.end(false);
                LOG_WARN("failed to regenerate ai column", K(tmp_ret), K(table_id), K(end_ret));
              } else if (OB_SUCCESS != (tmp_ret = trans.end(true))) {
                LOG_WARN("failed to commit regenerated ai column", K(tmp_ret), K(table_id));
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObCSAiColumnProcessor::process(common::ObIArray<ObCSRow> &rows)
{
  int ret = common::OB_SUCCESS;
  // AI columns are maintained asynchronously: the DML path registers a task row in
  // oceanbase.__all_ai_column_task inside the same transaction and ObAIColumnWorker
  // publishes the result under a version fence. Generating the same column here as
  // well would evaluate every AI expression twice (and race with the registered
  // tasks), so the change stream plugin does not generate AI columns.
  UNUSED(rows);
  return ret;
}

// ---------------------------------------------------------------------------
// ObCSPluginAiColumn
// ---------------------------------------------------------------------------

ObCSPluginAiColumn::ObCSPluginAiColumn()
  : ObCSPlugin(),
    is_inited_(false)
{
  set_plugin_type(CS_PLUGIN_AI_COLUMN);
}

ObCSPluginAiColumn::~ObCSPluginAiColumn()
{
  destroy();
}

int ObCSPluginAiColumn::init()
{
  int ret = common::OB_SUCCESS;
  if (is_inited_) {
    ret = common::OB_INIT_TWICE;
  } else {
    is_inited_ = true;
  }
  return ret;
}

void ObCSPluginAiColumn::destroy()
{
  is_inited_ = false;
}

int ObCSPluginAiColumn::process(common::ObIArray<ObCSRow> &rows, ObCSExecCtx &ctx)
{
  int ret = common::OB_SUCCESS;
  if (!is_inited_) {
    ret = common::OB_NOT_INIT;
  } else if (rows.count() == 0) {
    // nothing to do
  } else {
    ObCSAiColumnProcessor processor(ctx);
    ret = processor.process(rows);
  }
  return ret;
}

int ObCSPluginAiColumn::commit()
{
  return common::OB_SUCCESS;
}

ObCSPlugin *create_ai_column_plugin()
{
  return OB_NEW(ObCSPluginAiColumn, common::ObMemAttr("CSAiColumn"));
}

}  // namespace share
}  // namespace oceanbase
