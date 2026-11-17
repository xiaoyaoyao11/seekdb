"""Real-SQL reproductions for final AI COLUMN review findings.
Uses only a private parent-owned socket and a local recording model.
"""
import json,os,time,unittest,uuid
import pymysql
from test_ai_column_worker_integration import RecordingModel

class ReviewReproductions(unittest.TestCase):
    def setUp(self):
        self.db=pymysql.connect(unix_socket=os.environ['SEEKDB_TEST_SOCKET'],user='root',database='test',autocommit=True,charset='utf8mb4',read_timeout=30)
        self.addCleanup(self.db.close)
        self.http=RecordingModel();self.addCleanup(self.http.close)
        self.name='review_'+uuid.uuid4().hex[:12]
        self.model=self.name+'_model';self.endpoint=self.name+'_ep'
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',(self.model,json.dumps(dict(type='completion',model_name='fixture'))))
        self.addCleanup(self.sql,'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)',(self.model,))
        self.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',(self.endpoint,json.dumps(dict(ai_model_name=self.model,url=f'http://127.0.0.1:{self.http.server.server_port}/v1/chat/completions',provider='openai',access_key='local-fixture',request_model_name='fixture'))))
        self.addCleanup(self.sql,'CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)',(self.endpoint,))
        self.addCleanup(self.sql,f'DROP TABLE IF EXISTS `{self.name}`')
    def sql(self,text,args=None):
        with self.db.cursor() as cur:
            cur.execute(text,args);return cur.fetchall()
    def completed(self):
        deadline=time.monotonic()+25
        while time.monotonic()<deadline:
            if self.sql('SELECT COUNT(*) FROM oceanbase.__all_ai_column_task WHERE table_id=(SELECT table_id FROM oceanbase.__all_table WHERE table_name=%s)',(self.name,))==((0,),):return
            time.sleep(.05)
        self.fail('task did not reach terminal cleanup')
    def test_modify_change_ai_attribute_must_not_be_ignored(self):
        self.sql(f'CREATE TABLE `{self.name}`(id BIGINT PRIMARY KEY,body VARCHAR(100),answer VARCHAR(200))')
        original=self.sql(f'SHOW CREATE TABLE `{self.name}`')[0][1]
        for action in ['MODIFY COLUMN answer','CHANGE COLUMN answer answer']:
            with self.subTest(action=action):
                with self.assertRaises(pymysql.err.DatabaseError) as denied:
                    self.sql(f'ALTER TABLE `{self.name}` {action} VARCHAR(200) AI COLUMN(AI_COMPLETE(%s,body))',(self.model,))
                self.assertEqual(denied.exception.args[0],1235)
                self.assertEqual(self.sql(f'SHOW CREATE TABLE `{self.name}`')[0][1],original)
    def test_prompt_native_json_must_render_for_string_and_integer(self):
        self.sql(f'''CREATE TABLE `{self.name}`(id BIGINT PRIMARY KEY,body VARCHAR(100),
            answer TEXT AI COLUMN(AI_COMPLETE(%s,AI_PROMPT('Summarize {{0}}',body))) WITH(max_retries=0),
            score BIGINT AI COLUMN(AI_COMPLETE(%s,AI_PROMPT('Summarize {{0}}',body))) WITH(max_retries=0))''',(self.model,self.model))
        self.sql(f"INSERT INTO `{self.name}`(id,body) VALUES(1,'hi')")
        self.completed()
        self.assertEqual(len(self.http.events),2)
        self.assertEqual([e['user'] for e in self.http.events],['Summarize hi','Summarize hi'])
    def test_envelope_shaped_varchar_remains_literal(self):
        self.sql(f'CREATE TABLE `{self.name}`(id BIGINT PRIMARY KEY,body VARCHAR(300),answer TEXT AI COLUMN(AI_COMPLETE(%s,body)) WITH(max_retries=0))',(self.model,))
        literal=json.dumps(dict(template='Summarize {0}',args=['hi']))
        self.sql(f'INSERT INTO `{self.name}`(id,body) VALUES(1,%s)',(literal,))
        self.completed()
        self.assertEqual([e['user'] for e in self.http.events],[literal])
    def test_large_staged_group_does_not_block_group_or_unrelated_row(self):
        # Each normal fixture response is seven prefix bytes plus the input.
        # All five individual responses fit the supported 4 MiB result bound.
        limit=4*1024*1024
        prefix_bytes=len('result:'.encode('utf8'))
        columns=','.join(f'answer_{i} LONGTEXT AI COLUMN(AI_COMPLETE(%s,body)) WITH(max_retries=0)' for i in range(5))
        self.sql(f'CREATE TABLE `{self.name}`(id BIGINT PRIMARY KEY,body LONGTEXT,{columns})',(self.model,)*5)
        small=self.name+'_small'
        self.addCleanup(self.sql,f'DROP TABLE IF EXISTS `{small}`')
        self.sql(f'CREATE TABLE `{small}`(id BIGINT PRIMARY KEY,body VARCHAR(100),answer TEXT AI COLUMN(AI_COMPLETE(%s,body)) WITH(max_retries=0))',(self.model,))
        self.sql(f"INSERT INTO `{self.name}`(id,body) VALUES(1,REPEAT('x',%s))",(limit-prefix_bytes,))
        self.sql(f"INSERT INTO `{small}`(id,body) VALUES(1,'small-independent')")
        ready=' AND '.join(f'answer_{i} IS NOT NULL' for i in range(5))
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            group_ready=self.sql(f'SELECT {ready} FROM `{self.name}` WHERE id=1')==((1,),)
            small_ready=self.sql(f'SELECT answer IS NOT NULL FROM `{small}` WHERE id=1')==((1,),)
            if group_ready and small_ready:
                self.completed()
                sizes=self.sql(f'SELECT '+','.join(f'OCTET_LENGTH(answer_{i})' for i in range(5))+f' FROM `{self.name}` WHERE id=1')
                self.assertEqual(sizes,((limit,)*5,))
                return
            time.sleep(.1)
        self.fail('large-result group or unrelated work stalled; requests='+str(len(self.http.events)))

    def test_no_backslash_escapes_preserves_input_bytes(self):
        self.sql("SET SESSION sql_mode='NO_BACKSLASH_ESCAPES'")
        prefix="a\\b'"
        expression='CONCAT('+self.db.escape(prefix)+',body)'
        self.sql(f'CREATE TABLE `{self.name}`(id BIGINT PRIMARY KEY,body VARCHAR(100),answer TEXT AI COLUMN(AI_COMPLETE(%s,{expression})) WITH(max_retries=0))',(self.model,))
        self.sql(f"INSERT INTO `{self.name}`(id,body) VALUES(1,'hi')")
        self.completed()
        self.assertEqual([e['user'] for e in self.http.events],[prefix+'hi'])

if __name__=='__main__':unittest.main()
