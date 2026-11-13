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
 *
 * Change Stream plugin for asynchronous AI column maintenance.
 *
 * When a committed DML touches a table that has AI columns, the plugin asks the
 * SQL engine (through the inner SQL connection) to evaluate the AI expression
 * and publish the result:
 *
 *   UPDATE db.tbl SET ai_col = (<ai_expr>) WHERE ai_col IS NULL LIMIT N
 *
 * The inner session is allowed to write AI columns; user sessions are not.
 */

#ifndef OB_CS_PLUGIN_AI_COLUMN_H_
#define OB_CS_PLUGIN_AI_COLUMN_H_

#include "lib/ob_define.h"
#include "lib/container/ob_iarray.h"
#include "lib/hash/ob_hashmap.h"
#include "observer/change_stream/ob_change_stream_dispatcher.h"
#include "observer/change_stream/ob_change_stream_plugin.h"
#include "share/schema/ob_schema_getter_guard.h"

namespace oceanbase
{
namespace share
{

class ObCSAiColumnProcessor
{
public:
  explicit ObCSAiColumnProcessor(ObCSExecCtx &ctx);
  ~ObCSAiColumnProcessor();

  int process(common::ObIArray<ObCSRow> &rows);

private:
  int init_schema_guard_();
  int resolve_table_id_from_tablet_id_(const common::ObTabletID &tablet_id,
                                       uint64_t &table_id);
  int handle_table_(uint64_t table_id);

  ObCSExecCtx &ctx_;
  schema::ObSchemaGetterGuard schema_guard_;
  common::hash::ObHashMap<uint64_t, uint64_t> tablet_to_table_;
  common::hash::ObHashMap<uint64_t, int64_t> done_tables_;

  DISALLOW_COPY_AND_ASSIGN(ObCSAiColumnProcessor);
};

class ObCSPluginAiColumn : public ObCSPlugin
{
public:
  ObCSPluginAiColumn();
  virtual ~ObCSPluginAiColumn();

  virtual int init() override;
  virtual void destroy() override;
  virtual int process(common::ObIArray<ObCSRow> &rows, ObCSExecCtx &ctx) override;
  virtual int commit() override;

private:
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(ObCSPluginAiColumn);
};

ObCSPlugin *create_ai_column_plugin();

}  // namespace share
}  // namespace oceanbase

#endif  // OB_CS_PLUGIN_AI_COLUMN_H_
