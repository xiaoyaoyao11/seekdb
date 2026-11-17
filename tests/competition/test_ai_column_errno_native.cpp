#include "share/ob_errno.h"
#include <cstdio>
#include <cstring>
using namespace oceanbase::common;
int main()
{
  static_assert(OB_AI_COLUMN_UNFINISHED_TASKS == -11120);
  const char *format = ob_str_user_error(OB_AI_COLUMN_UNFINISHED_TASKS);
  char message[256] = {};
  if (format) snprintf(message, sizeof(message), format, 4242UL, 17UL);
  bool ok = ob_mysql_errno_with_check(OB_AI_COLUMN_UNFINISHED_TASKS) == 11120
      && !strcmp(ob_sqlstate(OB_AI_COLUMN_UNFINISHED_TASKS), "HY000")
      && !strcmp(ob_error_name(OB_AI_COLUMN_UNFINISHED_TASKS), "OB_AI_COLUMN_UNFINISHED_TASKS")
      && !strcmp(message, "AI column has unfinished tasks for table_id=4242, column_id=17")
      && ob_mysql_errno_with_check(OB_NOT_SUPPORTED) == 1235;
  int occurrences = 0;
  for (int error : g_all_ob_errnos) occurrences += error == OB_AI_COLUMN_UNFINISHED_TASKS;
  ok = ok && occurrences == 1;
  printf("%s SQL=%d state=%s message=%s unsupported=%d registry_occurrences=%d\n",
         ok ? "PASS" : "FAIL", ob_mysql_errno_with_check(OB_AI_COLUMN_UNFINISHED_TASKS),
         ob_sqlstate(OB_AI_COLUMN_UNFINISHED_TASKS), message,
         ob_mysql_errno_with_check(OB_NOT_SUPPORTED), occurrences);
  return ok ? 0 : 1;
}
