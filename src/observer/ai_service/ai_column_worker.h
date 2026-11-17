#pragma once
#include "rootserver/ob_server_thread_helper.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "lib/charset/ob_charset.h"
#include <string>
#include <vector>

namespace oceanbase {
namespace share { namespace schema { class ObMultiVersionSchemaService; } }
namespace sql { class ObSQLSessionInfo; }
namespace observer {
class ObInnerSQLConnection;
// native_proxy must use ObInnerSQLConnection (the observer's internal SQL
// proxy, never an external MySQL client). Publication scopes the DML owner's
// native-only ObSQLSessionInfo::set_ai_column_internal_write capability on the
// exact transaction connection and restores it before pool release.

class AiColumnWorker final : public rootserver::ObServerThreadHelper
{
public:
  AiColumnWorker() : proxy_(nullptr), schemas_(nullptr), lease_us_(0) {}
  ~AiColumnWorker() { destroy(); }
  int init(common::ObISQLClient &native_proxy,
           share::schema::ObMultiVersionSchemaService &schemas,
           int64_t concurrency = 3,
           int64_t lease_us = 600000000);
  // start/stop/wait are inherited; destroy also stops and joins all workers.
  void destroy() override;
  void deactivate() override { stop(); }
  int activate() override { return start(); }
  void do_work() override;
  // One bounded pass, useful for native service tests. No SQL/app-facing entry.
  int run_once();
private:
  struct Task {
    int64_t table = 0, column = 0, version = 0, state = 0, retries = 0;
    bool input_null = false, input_too_large = false, input_json = false;
    std::string rowkey, request, group, predicate, members, input, spec, token;
  };
  struct Target {
    enum Kind { INTEGER, JSON, STRING, VECTOR } kind = STRING;
    int64_t version = 0, max_retries = 0, dimension = 0, max_length = 0;
    common::ObCollationType collation = common::CS_TYPE_INVALID;
    bool length_in_bytes = false;
    std::string table, column, state, request, spec, model, predicate;
  };
  int load(common::ObISQLClient &, const std::string &, std::vector<Task> &);
  int target(const Task &, Target &, bool require_version = true,
             const sql::ObSQLSessionInfo *session = nullptr);
  int claim(Task &, bool &);
  int load_input(Task &);
  int load_staged(common::ObISQLClient &, const Task &, std::string &);
  int infer(const Task &, const Target &, std::string &);
  int validate(const Target &, std::string &);
  int stage(const Task &, const Target &, int error, const std::string &);
  int publish(const Task &);
  common::ObISQLClient *proxy_;
  share::schema::ObMultiVersionSchemaService *schemas_;

  int64_t lease_us_;
};
}}
