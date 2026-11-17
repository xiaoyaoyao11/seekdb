"""Run unchanged public file_sql_basic SQL/result with local fixture transport.

The official includes deploy fixtures using SSH/SCP and an admin TCP connection.
This socket-only harness replaces ONLY those two deployment/cleanup includes
with transport no-ops, provisioning the exact data directly. SQL case/result
bytes are read from Git HEAD and hash-checked; no scoring assertions are edited.
Requires PyMySQL and the actual bundled mysqltest binary.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import pymysql


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--socket',required=True)
    ap.add_argument('--source-root',required=True,type=Path)
    ap.add_argument('--output-dir',required=True,type=Path)
    ap.add_argument('--mysqltest',required=True,type=Path)
    args=ap.parse_args()
    out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    source=args.source_root.resolve()
    prefix='tools/deploy/mysql_test/test_suite/file_sql/'
    contents={}
    for extension,path in [('test','t/file_sql_basic.test'),('result','r/mysql/file_sql_basic.result')]:
        data=subprocess.check_output(['git','-C',str(source),'show','HEAD:'+prefix+path])
        (out/('file_sql_basic.'+extension)).write_bytes(data)
        contents[extension]=hashlib.sha256(data).hexdigest()
    data_dir=out/'data_for_mysqltest_file_sql';data_dir.mkdir()
    for name in ['basic.csv','basic.jsonl','invalid.txt','simple.parquet.b64']:
        data=subprocess.check_output(['git','-C',str(source),'show','HEAD:'+prefix+'data/'+name])
        if name.endswith('.b64'): (data_dir/'simple.parquet').write_bytes(base64.b64decode(data))
        else: (data_dir/name).write_bytes(data)
    includes=out/'mysql_test/test_suite/file_sql/include';includes.mkdir(parents=True)
    for name in ['copy_file_sql_data.inc','remove_file_sql_data.inc']:
        (includes/name).write_text('# Local fixture transport handled by Python; test SQL unchanged.\n')
    wrapper=out/'wrapper.test'
    wrapper.write_text('--disable_query_log\nlet $OBSERVER_DIR = '+str(out)+';\n--source '+str(out/'file_sql_basic.test')+'\n')
    connection=pymysql.connect(unix_socket=args.socket,user='root',database='test',autocommit=True)
    with connection.cursor() as cursor:
        cursor.execute('SELECT @@global.secure_file_priv');row=cursor.fetchone()
        assert row is not None
        previous=row[0]
        cursor.execute('SET GLOBAL secure_file_priv=%s',(str(out),))
    try:
        command=[str(args.mysqltest.resolve()),'--no-defaults','--user=root','--socket='+args.socket,
            '--database=test','--default-character-set=utf8mb4','--logdir='+str(out),
            '--test-file='+str(wrapper),'--result-file='+str(out/'file_sql_basic.result')]
        run=subprocess.run(command,cwd=out,capture_output=True,text=True,timeout=180)
        print(run.stdout+run.stderr,flush=True)
        report={'returncode':run.returncode,'case_hashes':contents,'fixture_transport':'local, adapted includes only',
            'argv':command,'stdout':run.stdout,'stderr':run.stderr}
        for extension in contents:
            assert hashlib.sha256((out/('file_sql_basic.'+extension)).read_bytes()).hexdigest()==contents[extension]
        (out/'report.json').write_text(json.dumps(report,indent=2))
        return run.returncode
    finally:
        with connection.cursor() as cursor:cursor.execute('SET GLOBAL secure_file_priv=%s',(previous,))
        connection.close();shutil.rmtree(data_dir)


if __name__=='__main__':raise SystemExit(main())
