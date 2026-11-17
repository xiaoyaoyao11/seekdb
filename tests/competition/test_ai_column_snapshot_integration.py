"""Deterministic RC race regression against a parent-owned native server.

Requires SEEKDB_TEST_SOCKET and SEEKDB_SNAPSHOT_BARRIER, an external debugger
controller executable accepting: arm <connection_id> <phase>, wait, release.
`wait` must return only after the selected session is suspended, with all other
sessions still runnable (e.g. GDB non-stop mode). Phases:
  before_start_stmt: final resolution finished, outer start_stmt not yet called;
  after_snapshot: outer snapshot acquired, before strict validation/first scan.
The controller must target only the outer statement, not nested strict probes.
No SQL variable, error simulation, sleep, or production test bypass is used.
Without that apparatus these tests SKIP, not claim race coverage.
"""
import os
import shlex
import subprocess
import threading
import unittest

import pymysql
import test_ai_column_strict_retry_integration as strict_suite


@unittest.skipUnless(os.environ.get('SEEKDB_TEST_SOCKET') and
                     os.environ.get('SEEKDB_SNAPSHOT_BARRIER'),
                     'native socket and external debugger barrier required')
class AIColumnSnapshotIntegration(strict_suite.AIColumnStrictRetry):
    # Do not inherit the ordinary suite's test methods a second time.
    def barrier(self, action, *args):
        subprocess.run(shlex.split(os.environ['SEEKDB_SNAPSHOT_BARRIER']) +
                       [action, *map(str, args)], check=True, timeout=45)

    def race(self, query, phase, expected_error):
        if not os.environ.get('SEEKDB_SNAPSHOT_BARRIER'):
            self.skipTest('external non-stop debugger barrier required')
        self.ready_baseline()
        self.sql('SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED')
        self.sql('SET SESSION ai_column_strict_read=ON')
        self.sql('START TRANSACTION')
        writer = pymysql.connect(unix_socket=os.environ['SEEKDB_TEST_SOCKET'],
            user='root', database='test', autocommit=True, charset='utf8mb4',
            read_timeout=30, write_timeout=30)
        self.addCleanup(writer.close)
        outcome = []
        connection_id = self.sql('SELECT CONNECTION_ID()')[0][0]
        self.barrier('arm', connection_id, phase)
        def read():
            try:
                outcome.append(('ok', self.sql(query)))
            except pymysql.err.DatabaseError as exc:
                outcome.append(('error', exc.args))
        thread = threading.Thread(target=read, daemon=True)
        thread.start()
        try:
            self.barrier('wait')
            # RecordingModel deterministically fails prompts containing 'fail';
            # the worker may publish FAILED, never READY, after this commit.
            with writer.cursor() as cursor:
                cursor.execute(f"UPDATE `{self.table}` SET body='fail-dependency-changed' WHERE id=1")
                cursor.execute(f'SELECT result FROM `{self.table}` WHERE id=1')
                self.assertEqual(cursor.fetchall(), ((None,),))
        finally:
            self.barrier('release')
            thread.join(40)
        self.assertFalse(thread.is_alive(), 'reader failed to finish after release')
        self.assertEqual(len(outcome), 1, outcome)
        if expected_error:
            self.assertEqual(outcome[0][0], 'error', outcome)
            self.assertEqual(outcome[0][1][0], 11120, outcome)
            self.barrier('assert_clean')
        else:
            self.assertEqual(outcome[0][0], 'ok', outcome)
            self.assertTrue(outcome[0][1], outcome)
            self.assertIsNotNone(outcome[0][1][0][0], outcome)
        # A rejected UPDATE/DELETE must not alter ordinary data; rollback does
        # not undo the other session's committed dependency invalidation.
        self.db.rollback()
        self.assertEqual(self.sql(f'SELECT category FROM `{self.table}` WHERE id=1'), (('news',),))

    def test_ready_resolve_then_pending_commit_before_select_snapshot(self):
        self.race(f'SELECT result FROM `{self.table}` WHERE id=1', 'before_start_stmt', True)

    def test_ready_resolve_then_pending_commit_before_insert_select_snapshot(self):
        self.db.rollback()
        sink = self.table + '_sink'
        self.sql(f'CREATE TABLE `{sink}` (id BIGINT PRIMARY KEY, result VARCHAR(200))')
        self.addCleanup(self.sql, f'DROP TABLE IF EXISTS `{sink}`')
        self.race(f'INSERT INTO `{sink}` SELECT id,result FROM `{self.table}` WHERE id=1',
                  'before_start_stmt', True)
        self.assertEqual(self.sql(f'SELECT COUNT(*) FROM `{sink}`'), ((0,),))

    def test_ready_resolve_then_pending_commit_before_update_snapshot(self):
        self.race(f"UPDATE `{self.table}` SET category='changed' WHERE result IS NOT NULL",
                  'before_start_stmt', True)

    def test_ready_resolve_then_pending_commit_before_delete_subquery_snapshot(self):
        self.race(f'DELETE FROM `{self.table}` WHERE EXISTS '
                  f'(SELECT result FROM `{self.table}` q WHERE q.id=1)',
                  'before_start_stmt', True)

    def test_strict_rejection_clears_native_scope_and_reserved_snapshot(self):
        self.db.rollback()
        self.sql('SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED')
        self.sql(f"INSERT INTO `{self.table}`(id,body,category) VALUES(1,'fail-cleanup','original')")
        self.sql('SET SESSION ai_column_strict_read=ON')
        for explicit in (False, True):
            with self.subTest(explicit_transaction=explicit):
                if explicit:
                    self.sql('START TRANSACTION')
                    self.sql(f"UPDATE `{self.table}` SET category='keep-own-write' WHERE id=1")
                connection_id = self.sql('SELECT CONNECTION_ID()')[0][0]
                self.barrier('arm', connection_id, 'before_start_stmt')
                outcome = []
                def reject():
                    try:
                        outcome.append(('ok', self.sql(f'SELECT result FROM `{self.table}` WHERE id=1')))
                    except pymysql.err.DatabaseError as exc:
                        outcome.append(('error', exc.args))
                thread = threading.Thread(target=reject, daemon=True)
                thread.start()
                try:
                    self.barrier('wait')
                finally:
                    self.barrier('release')
                    thread.join(40)
                self.assertFalse(thread.is_alive())
                self.assertEqual(len(outcome), 1, outcome)
                self.assertEqual(outcome[0][0], 'error', outcome)
                self.assertEqual(outcome[0][1][0], 11120, outcome)
                # Inspect the actual native session before ANY follow-up SQL
                # could incidentally clean the leaked statement/snapshot.
                self.barrier('assert_clean')
                if explicit:
                    self.assertEqual(self.sql(f'SELECT category FROM `{self.table}` WHERE id=1'),
                                     (('keep-own-write',),))
                    self.db.rollback()
                    self.assertEqual(self.sql(f'SELECT category FROM `{self.table}` WHERE id=1'),
                                     (('original',),))

    def test_commit_after_snapshot_does_not_change_probe_snapshot(self):
        self.race(f'SELECT result FROM `{self.table}` WHERE id=1', 'after_snapshot', False)


# Inherited setup/helpers are reused, but inherited test cases belong to their
# original suite. unittest discovers methods through getattr, so mask them.
for _name in dir(strict_suite.AIColumnStrictRetry):
    if _name.startswith('test_'):
        setattr(AIColumnSnapshotIntegration, _name, None)

if __name__ == '__main__':
    unittest.main()
