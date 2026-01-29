#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import os
import wttest
from wtscenario import make_scenarios

# test_log_truncate.py
#   Verify log file truncation at clean shutdown
class test_log_truncate(wttest.WiredTigerTestCase):
    """
    Test that log files are truncated to actual data size at clean shutdown,
    and that recovery still works correctly with truncated files.
    """
    # Use small log file to make tests faster
    conn_config = 'log=(enabled=true,file_max=1M)'

    scenarios = make_scenarios([
        ('with_checkpoint', dict(checkpoint=True)),
        ('without_checkpoint', dict(checkpoint=False)),
    ])

    def get_log_files(self):
        """Return list of log files and their sizes."""
        log_files = []
        for f in os.listdir('.'):
            if f.startswith('WiredTigerLog.'):
                log_files.append((f, os.path.getsize(f)))
        return sorted(log_files)

    def test_truncate_size(self):
        """Verify log file is truncated after clean shutdown."""
        uri = 'table:test_trunc'
        self.session.create(uri, 'key_format=S,value_format=S')

        # Insert some data
        cursor = self.session.open_cursor(uri)
        for i in range(100):
            cursor[str(i)] = 'value' * 100
        cursor.close()

        if self.checkpoint:
            self.session.checkpoint()

        # Close cleanly
        self.close_conn()

        # Check log file sizes - should be much smaller than 1MB (1048576 bytes)
        log_files = self.get_log_files()
        self.assertTrue(len(log_files) > 0, "No log files found")

        for name, size in log_files:
            # Allow up to 200KB for actual data, but should be much less than 1MB
            self.assertLess(size, 200000,
                f"Log file {name} not truncated: {size} bytes (expected < 200KB)")

    def test_recovery_after_truncate(self):
        """Verify recovery works correctly with truncated log files."""
        uri = 'table:test_recovery'
        self.session.create(uri, 'key_format=i,value_format=S')

        nrows = 500

        # Insert data and checkpoint
        cursor = self.session.open_cursor(uri)
        for i in range(nrows):
            cursor[i] = 'value_%d' % i
        cursor.close()
        self.session.checkpoint()

        # Insert more data after checkpoint (needs log recovery)
        cursor = self.session.open_cursor(uri)
        for i in range(nrows, nrows * 2):
            cursor[i] = 'value_%d' % i
        cursor.close()

        # Close and reopen (triggers recovery with truncated log)
        self.reopen_conn()

        # Verify all data present
        cursor = self.session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, 'value_%d' % k)
            count += 1
        self.assertEqual(count, nrows * 2)
        cursor.close()

    def test_multiple_restart_cycles(self):
        """Stress test: multiple shutdown/restart cycles."""
        uri = 'table:test_cycles'

        for cycle in range(10):
            if cycle == 0:
                self.session.create(uri, 'key_format=i,value_format=S')

            cursor = self.session.open_cursor(uri)
            base = cycle * 50
            for i in range(50):
                cursor[base + i] = 'cycle_%d_value_%d' % (cycle, i)
            cursor.close()

            if cycle % 2 == 0:
                self.session.checkpoint()

            self.reopen_conn()

            # Verify data integrity
            cursor = self.session.open_cursor(uri)
            count = sum(1 for _ in cursor)
            cursor.close()
            self.assertEqual(count, (cycle + 1) * 50)

    def test_file_grows_after_truncate(self):
        """Verify truncated file can grow again on next session."""
        uri = 'table:test_grow'
        self.session.create(uri, 'key_format=i,value_format=S')

        # Small insert, close (truncates)
        cursor = self.session.open_cursor(uri)
        for i in range(10):
            cursor[i] = 'small'
        cursor.close()
        self.close_conn()

        # Get total log size after first session
        log_files_first = self.get_log_files()
        total_size_first = sum(size for _, size in log_files_first)
        self.assertGreater(total_size_first, 0, "Log files should exist after first session")

        # Reopen and insert more data (not too much to avoid rotation complexity)
        self.open_conn()
        cursor = self.session.open_cursor(uri)
        # Insert moderate amount of data
        for i in range(10, 100):
            cursor[i] = 'x' * 1000
        cursor.close()
        self.close_conn()

        # Total log size should have increased
        log_files_second = self.get_log_files()
        total_size_second = sum(size for _, size in log_files_second)

        self.assertGreater(total_size_second, total_size_first,
            f"Total log size should grow: {total_size_second} vs {total_size_first}")

        # Verify data
        self.open_conn()
        cursor = self.session.open_cursor(uri)
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 100)


class test_log_truncate_rotation(wttest.WiredTigerTestCase):
    """Test truncation with log file rotation."""

    # Very small log file to force rotation. Disable log removal so we can
    # inspect multiple log files after the test.
    conn_config = 'log=(enabled=true,file_max=100K,remove=false)'

    def test_rotation_and_truncate(self):
        """Test truncation works correctly with multiple log files."""
        uri = 'table:test_rotation'
        self.session.create(uri, 'key_format=i,value_format=S')

        # Insert enough to cause multiple rotations
        cursor = self.session.open_cursor(uri)
        large_value = 'x' * 1000
        for i in range(300):
            cursor[i] = large_value
        cursor.close()

        self.session.checkpoint()
        self.close_conn()

        # Should have multiple log files
        log_files = []
        for f in os.listdir('.'):
            if f.startswith('WiredTigerLog.'):
                log_files.append((f, os.path.getsize(f)))

        self.assertGreater(len(log_files), 1, "Expected multiple log files")

        # The last (active) log file should be truncated to less than file_max
        log_files.sort()
        last_log = log_files[-1]
        self.assertLess(last_log[1], 102400,
            f"Last log file {last_log[0]} not truncated: {last_log[1]} bytes")

        # Reopen and verify
        self.open_conn()
        cursor = self.session.open_cursor(uri)
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 300)


if __name__ == '__main__':
    wttest.run()
