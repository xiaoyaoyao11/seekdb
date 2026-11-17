"""Native full-TU Parquet test runner. Install pyarrow ONLY in a test venv.
Usage: python tests/competition/run_file_parquet_native.py [--public-only]
Compiler: CXX env or g++; no dependency manifests or server required.
"""
import base64
import datetime
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import pyarrow as pa
import pyarrow.parquet as pq
ROOT = Path(__file__).resolve().parents[2]
scratch = Path('F:/Hermes-Official/cache/scratch')
OUT = Path(os.environ.get('TMPDIR', str(scratch) if scratch.exists() else tempfile.gettempdir())) / 'seekdb-parquet-native'
OUT.mkdir(parents=True, exist_ok=True)
EXE = OUT / ('parquet_test.exe' if os.name == 'nt' else 'parquet_test')

def expectation(path, table):
    lines = [f'{table.num_columns} {table.num_rows}']
    for field in table.schema:
        t = field.type
        kind = 3 if pa.types.is_boolean(t) else 1 if pa.types.is_integer(t) else 2 if pa.types.is_floating(t) else 5 if pa.types.is_date(t) else 6 if pa.types.is_timestamp(t) else 4
        lines.append(f'{field.name.encode().hex()} {kind} {int(field.nullable)}')
    cols = [c.to_pylist() for c in table.columns]
    for i in range(table.num_rows):
        cells = []
        for col in cols:
            v = col[i]
            if v is None: cells.append('N'); continue
            if isinstance(v, bool): v = 'true' if v else 'false'
            elif isinstance(v, datetime.datetime): v = v.strftime('%Y-%m-%d %H:%M:%S') + (f'.{v.microsecond:06}' if v.microsecond else '')
            elif isinstance(v, datetime.date): v = v.isoformat()
            elif isinstance(v, float): v = format(v, '.17g')
            cells.append('V' + str(v).encode().hex())
        lines.append(' '.join(cells))
    target = path.with_suffix('.expected'); target.write_text('\n'.join(lines)+'\n'); return target

def run(path, table=None, projection=None):
    expected = expectation(path, table) if table is not None else 'reject'
    cmd = [str(EXE), str(path), str(expected)]
    if projection is not None: cmd.append(projection)
    subprocess.run(cmd, check=True)

subprocess.run([os.environ.get('CXX','g++'), '-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
               '-I'+str(ROOT/'src'), '-I'+str(ROOT/'src/oblib'),
               str(ROOT/'src/sql/engine/basic/file_sql_parquet.cpp'),
               str(ROOT/'tests/competition/test_file_parquet_native.cpp'), '-o', str(EXE)], check=True)
public = OUT/'public.parquet'
public.write_bytes(base64.b64decode((ROOT/'tools/deploy/mysql_test/test_suite/file_sql/data/simple.parquet.b64').read_bytes()))
run(public, pq.read_table(public))

if '--public-only' not in sys.argv:
    table = pa.Table.from_arrays([
        pa.array([-(1<<63), None, (1<<63)-1, -7, 0, 9, 9, 9, 9], pa.int64()),
        pa.array([True, None, False, True, True, True, False, False, True]),
        pa.array([1.25, None, -3.5, 0, 2.5, 2.5, 2.5, 2.5, 2.5], pa.float64()),
        pa.array(['hello', '', None, '世界', 'x'*1000, 'hello', 'hello', 'hello', 'hello']),
        pa.array([1.5, None, -2.5, 0, 8, 8, 8, 8, 8], pa.float32()),
    ], names=['large_int', 'flag', 'number', 'words', 'float32'])
    p = OUT/'primitives.parquet'
    pq.write_table(table,p,row_group_size=4,compression='snappy',data_page_version='1.0')
    run(p,table)
    run(p,table,'3 0')
    temporal = pa.table({
        'day': pa.array([datetime.date(1,1,1),datetime.date(1969,12,31),None,datetime.date(2000,2,29),datetime.date(9999,12,31)],pa.date32()),
        'millis': pa.array([-1,0,None,1234567890123,1234567890123],pa.timestamp('ms')),
        'micros': pa.array([-1,0,None,1234567890123456,1234567890123456],pa.timestamp('us')),
        'nanos': pa.array([-1000,0,None,1234567890123456000,1234567890123456000],pa.timestamp('ns')),
    })
    p=OUT/'temporal.parquet';pq.write_table(temporal,p,compression='snappy',data_page_version='2.0',row_group_size=3)
    run(p,temporal)
    null_table=pa.table({'all_null':pa.array([None]*17,pa.int32()), 'string_null':pa.array([None]*17,pa.string())})
    for version in ['1.0','2.0']:
        p=OUT/f'null-{version}.parquet';pq.write_table(null_table,p,data_page_version=version)
        run(p,null_table)
    # More rows than write_batch_size and data_page_size: genuinely multiple pages.
    many=pa.Table.from_arrays([
        pa.array([None if i%11==0 else i*31-80000 for i in range(5003)],pa.int32()),
        pa.array([None if i%13==0 else ('世界-'+str(i%97))*5 for i in range(5003)]),
        pa.array([i%3!=0 for i in range(5003)]),
    ], schema=pa.schema([pa.field('id',pa.int32()),pa.field('label',pa.string()),pa.field('required_flag',pa.bool_(),nullable=False)]))
    for version in ['1.0','2.0']:
        for codec in ['NONE','snappy']:
            for dictionary in [False,True]:
                p=OUT/f'pages-{version}-{codec}-{dictionary}.parquet'
                pq.write_table(many,p,data_page_version=version,compression=codec,use_dictionary=dictionary,row_group_size=2301,data_page_size=1024,write_batch_size=128,dictionary_pagesize_limit=512,write_page_checksum=True)
                run(p,many)
                run(p,many,'2 0')
    ints=pa.table({
        'i8':pa.array([-128,None,127],pa.int8()),
        'i16':pa.array([-32768,None,32767],pa.int16()),
        'u8':pa.array([0,None,255],pa.uint8()),
        'u16':pa.array([0,None,65535],pa.uint16()),
        'u32':pa.array([0,None,(1<<32)-1],pa.uint32()),
    })
    p=OUT/'narrow-integers.parquet';pq.write_table(ints,p);run(p,ints)
