#pragma once
#include "share/schema/ob_schema_getter_guard.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase { namespace sql {
// Check before schema inference: secure_file_priv is a path boundary, not a grant.
// Use the native schema guard so active roles and revoked grants are respected.
inline int file_sql_check_file_privilege(share::schema::ObSchemaGetterGuard &guard,
                                         const ObSQLSessionInfo &session)
{
  using namespace share::schema;
  common::ObArenaAllocator allocator;
  ObStmtNeedPrivs privileges(allocator);
  ObNeedPriv need;
  need.priv_level_=OB_PRIV_USER_LEVEL;
  need.priv_set_=OB_PRIV_FILE;
  int ret=privileges.need_privs_.reserve(1);
  if (OB_SUCC(ret)) ret=privileges.need_privs_.push_back(need);
  if (OB_SUCC(ret)) {
    ObSessionPrivInfo session_priv;
    if (OB_SUCC(ret=session.get_session_priv_info(session_priv))) {
      ret=guard.check_priv(session_priv, session.get_enable_role_array(), privileges);
    }
  }
  return ret;
}
} }
