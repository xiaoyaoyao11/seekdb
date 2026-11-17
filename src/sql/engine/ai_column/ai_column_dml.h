#pragma once
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase { namespace sql {
// Scope this on the actual native connection's session, not a detached session.
// This capability is neither serialized nor represented by a SQL variable.
class AiColumnInternalWriteGuard
{
public:
  explicit AiColumnInternalWriteGuard(ObSQLSessionInfo &session)
      : session_(session), saved_(session.is_ai_column_internal_write())
  { session_.set_ai_column_internal_write(true); }
  ~AiColumnInternalWriteGuard() { session_.set_ai_column_internal_write(saved_); }
private:
  ObSQLSessionInfo &session_;
  bool saved_;
  AiColumnInternalWriteGuard(const AiColumnInternalWriteGuard &) = delete;
  AiColumnInternalWriteGuard &operator=(const AiColumnInternalWriteGuard &) = delete;
};

class AiColumnDml
{
public:
  // Runs only after successful ordinary DAS row modification; all nested writes
  // use the caller's external-session connection and transaction/savepoint.
  static int after_row(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef,
                       ObDmlEventType event);
  // Public canonical key contract for the worker and retry executor. SQL
  // predicate is diagnostic; binary identity is ObRowkey::serialize bytes.
  static int row_identity(const share::schema::ObTableSchema &table,
                          const ObDMLBaseCtDef &ctdef, const ExprFixedArray &row,
                          ObEvalCtx &eval_ctx, common::ObIAllocator &allocator,
                          common::ObString &binary, common::ObSqlString &predicate);
};
}} // namespace oceanbase::sql
