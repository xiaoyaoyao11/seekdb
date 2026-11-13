# Install the fixtures of the join and security test.  The fixtures live in
# their own directory so that the glob of the basic test keeps seeing exactly
# the four files of the official fixture directory.
connect (file_sql_join_setup,$OBMYSQL_MS0,admin,$OBMYSQL_PWD,test,$OBMYSQL_PORT);
connection file_sql_join_setup;

--disable_query_log
let $FILE_SQL_HOST = query_get_value('select host_ip()',host_ip(), 1);
let $FILE_SQL_JOIN_DATA_DIR = $OBSERVER_DIR/data_for_mysqltest_file_sql_join;
let $FILE_SQL_JOIN_COPY_CMD = ssh $FILE_SQL_HOST 'rm -rf "$FILE_SQL_JOIN_DATA_DIR" && mkdir -p "$FILE_SQL_JOIN_DATA_DIR"' && scp -q $PWD/mysql_test/test_suite/file_sql/data_join/* $FILE_SQL_HOST:"$FILE_SQL_JOIN_DATA_DIR/";
exec $FILE_SQL_JOIN_COPY_CMD;
--enable_query_log

disconnect file_sql_join_setup;
connection default;
