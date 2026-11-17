"""External GDB/MI non-stop barrier for a private, parent-owned seekdb process.
No SQL test hook or production behavioral switch. CLI: --control PATH arm ID PHASE;
wait/release/shutdown. Server: --serve --pid PID --binary PATH --control PATH.
"""
import argparse,json,os,re,socket,subprocess,threading,time

class MI:
    def __init__(self,binary,pid):
        self.process=subprocess.Popen(['gdb','--quiet','--nx','--interpreter=mi2',binary],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1)
        assert self.process.stdin is not None and self.process.stdout is not None
        self.session_address=''
        self.lock=threading.Lock();self.pending={};self.sequence=0;self.hit=threading.Event();self.stopped=None;self.breakpoint=None;self.errors=[]
        self.reader=threading.Thread(target=self.read,daemon=True);self.reader.start()
        for cmd in ['-gdb-set pagination off','-gdb-set confirm off','-gdb-set print thread-events off','-gdb-set auto-solib-add off','-gdb-set mi-async on','-gdb-set non-stop on',f'-target-attach {pid}','-exec-continue --all']:
            self.command(cmd)
    def read(self):
        assert self.process.stdout is not None
        for line in self.process.stdout:
            line=line.strip()
            match=re.match(r'(\d+)\^(.*)',line)
            if match:
                with self.lock:
                    entry=self.pending.get(int(match[1]))
                    if entry:entry[1].append(match[2]);entry[0].set()
            elif line.startswith('*stopped'):
                tid=re.search(r'thread-id="([^"]+)"',line)
                bp=re.search(r'bkptno="([^"]+)"',line)
                if bp and bp[1]==self.breakpoint and tid:
                    self.stopped=tid[1];self.hit.set()
                elif 'reason="exited' not in line:
                    signal_name=re.search(r'signal-name="([^"]+)"',line)
                    if signal_name and signal_name[1]=='0' and tid:
                        # Linux non-stop attach may report late PTRACE_EVENT_STOP
                        # notifications for individual threads. They are debugger
                        # bookkeeping, not a database fault or the requested barrier.
                        def resume(thread_id=tid[1]):
                            try:self.command('-exec-continue --thread '+thread_id)
                            except RuntimeError as error:
                                if 'running' not in str(error):self.errors.append(str(error))
                        threading.Thread(target=resume,daemon=True).start()
                    else:
                        self.errors.append(line)
                        reason=re.search(r'reason="([^"]+)"',line)
                        print('UNEXPECTED_GDB_STOP',reason[1] if reason else '?',signal_name[1] if signal_name else '?',flush=True)
    def command(self,text):
        assert self.process.stdin is not None
        with self.lock:
            self.sequence+=1;token=self.sequence;event=threading.Event();result=[];self.pending[token]=(event,result)
            self.process.stdin.write(str(token)+text+'\n');self.process.stdin.flush()
        if not event.wait(45):raise RuntimeError('GDB response timeout: '+text)
        with self.lock:self.pending.pop(token,None)
        if result[0].startswith('error'):raise RuntimeError(result[0])
        return result[0]
    def arm(self,connection,phase):
        if self.stopped:raise RuntimeError('release the current stopped thread first')
        if self.breakpoint:self.command('-break-delete '+self.breakpoint)
        self.hit.clear()
        functions={'before_start_stmt':'oceanbase::sql::ObSqlTransControl::start_stmt',
                   'after_snapshot':'oceanbase::sql::AiColumnStrictRead::check_statement'}
        if phase not in functions:raise ValueError('unknown barrier phase')
        # A source-line breakpoint can include inline local-class ctor/dtor
        # locations where exec_ctx is out of scope. Use the single static-method
        # entry, with its first reference argument in the x86-64 SysV RDI register.
        address=self.command('-data-evaluate-expression '+json.dumps('(void*)&'+functions[phase]))
        match=re.search(r'value="(0x[0-9a-fA-F]+)',address)
        if not match:raise RuntimeError('no native function entry: '+address)
        result=self.command('-break-insert -d *'+match[1])
        match=re.search(r'number="([^"]+)"',result)
        if not match:raise RuntimeError('GDB did not create breakpoint: '+result)
        self.breakpoint=match[1]
        ctx='((oceanbase::sql::ObExecContext*)$rdi)'
        condition=f'{ctx}->my_session_ != 0 && {ctx}->my_session_->sessid_ == {int(connection)} && {ctx}->parent_ctx_ == 0'
        try:
            self.command('-break-condition '+self.breakpoint+' '+condition)
            self.expected_connection=int(connection);self.phase=phase
            self.command('-break-enable '+self.breakpoint)
        except Exception:
            self.release();raise
    def release(self):
        if self.breakpoint:
            self.command('-break-delete '+self.breakpoint);self.breakpoint=None
        if self.stopped:
            tid=self.stopped;self.stopped=None;self.command('-exec-continue --thread '+tid)
    def close(self):
        try:self.release();self.command('-target-detach');self.command('-gdb-exit')
        finally:
            try:self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait()

def serve(args):
    mi=MI(args.binary,args.pid)
    try:
        with socket.socket(getattr(socket,'AF_UNIX'),socket.SOCK_STREAM) as listener:
            listener.bind(args.control);os.chmod(args.control,0o600);listener.listen(4)
            print('SNAPSHOT_BARRIER_READY',flush=True)
            running=True
            while running:
                with listener.accept()[0] as client:
                    request=json.loads(client.makefile('r').readline());action=request[0]
                    try:
                        if action=='arm':mi.arm(request[1],request[2])
                        elif action=='wait':
                            if not mi.hit.wait(25):raise RuntimeError('selected outer statement did not hit barrier; '+repr(mi.errors))
                            if not mi.stopped:raise RuntimeError('barrier has no stopped thread')
                            connection=mi.command('-data-evaluate-expression --thread '+mi.stopped+' '+json.dumps('((oceanbase::sql::ObExecContext*)$rdi)->my_session_->sessid_'))
                            match=re.search(r'value="([0-9]+)"',connection)
                            if not match or int(match[1])!=mi.expected_connection:raise RuntimeError('wrong session stopped: '+connection)
                            pointer=mi.command('-data-evaluate-expression --thread '+mi.stopped+' '+json.dumps('(void*)((oceanbase::sql::ObExecContext*)$rdi)->my_session_'))
                            match=re.search(r'value="(0x[0-9a-fA-F]+)',pointer)
                            if not match:raise RuntimeError('cannot capture session pointer: '+pointer)
                            mi.session_address=match[1]
                            print('BARRIER_HIT',mi.phase,mi.expected_connection,flush=True)
                        elif action=='assert_clean':
                            session='((oceanbase::sql::ObSQLSessionInfo*)'+mi.session_address+')'
                            nested=mi.command('-data-evaluate-expression '+json.dumps(session+'->nested_count_'))
                            snapshot=mi.command('-data-evaluate-expression '+json.dumps(session+'->reserved_read_snapshot_version_.val_'))
                            if 'value="-1"' not in nested or 'value="18446744073709551615"' not in snapshot:
                                raise RuntimeError('unbalanced rejected statement: '+nested+'; '+snapshot)
                            print('REJECTED_STATEMENT_CLEAN',mi.expected_connection,flush=True)
                        elif action=='release':mi.release()
                        elif action=='shutdown':running=False
                        else:raise ValueError('unknown action')
                        response={'ok':True}
                    except Exception as exc:response={'ok':False,'error':str(exc)}
                    client.sendall((json.dumps(response)+'\n').encode())
    finally:
        mi.close()
        if os.path.exists(args.control):os.unlink(args.control)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serve',action='store_true');parser.add_argument('--pid',type=int);parser.add_argument('--binary');parser.add_argument('--control',required=True);parser.add_argument('action',nargs='*')
    args=parser.parse_args()
    if args.serve:
        if not args.pid or not args.binary:parser.error('serve requires pid and binary')
        return serve(args)
    with socket.socket(getattr(socket,'AF_UNIX'),socket.SOCK_STREAM) as client:
        client.settimeout(40);client.connect(args.control);client.sendall((json.dumps(args.action)+'\n').encode());response=json.loads(client.makefile('r').readline())
    if not response['ok']:raise RuntimeError(response['error'])

if __name__=='__main__':main()
