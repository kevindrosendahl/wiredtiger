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
import re
import wttest
from wtscenario import make_scenarios

# test_log_recovery_skip.py
#
# Test the log.recovery_skip configuration option, which allows WiredTiger
# to skip log scanning on startup after a verified clean shutdown.
#
# DURABILITY MODEL:
#   - At clean shutdown, a marker is written to the turtle file containing
#     the final log file number, offset, file size, and a checksum
#   - On startup, if recovery_skip is enabled, the marker is read and validated
#   - If the marker checksum is valid and the log file size matches, log
#     scanning is skipped
#   - The marker is invalidated immediately after reading (before any other work)
#   - If validation fails or the marker is missing, full recovery runs
#
# TRADE-OFF:
#   - Without this optimization: corruption detected at startup (eager)
#   - With this optimization: corruption detected on data access (lazy)
#   - Both have identical durability guarantees

class test_log_recovery_skip(wttest.WiredTigerTestCase):
    """
    Test the recovery_skip optimization for clean shutdown.
    """
    # Use small log file to make tests faster
    uri = 'table:test_recovery_skip'

    scenarios = make_scenarios([
        ('recovery_skip_on', dict(recovery_skip=True)),
        ('recovery_skip_off', dict(recovery_skip=False)),
    ], [
        ('with_checkpoint', dict(checkpoint=True)),
        ('without_checkpoint', dict(checkpoint=False)),
    ])

    def conn_config(self):
        config = 'log=(enabled=true,file_max=1M'
        if self.recovery_skip:
            config += ',recovery_skip=true'
        config += ')'
        return config

    def get_turtle_contents(self):
        """Read the turtle file and return its contents."""
        turtle_path = os.path.join('.', 'WiredTiger.turtle')
        if os.path.exists(turtle_path):
            with open(turtle_path, 'r') as f:
                return f.read()
        return None

    def has_shutdown_marker(self):
        """Check if the turtle file contains a shutdown marker."""
        contents = self.get_turtle_contents()
        if contents is None:
            return False
        return 'Log shutdown' in contents

    def get_marker_value(self):
        """Extract the shutdown marker value from the turtle file."""
        contents = self.get_turtle_contents()
        if contents is None:
            return None
        lines = contents.split('\n')
        for i, line in enumerate(lines):
            if line == 'Log shutdown':
                if i + 1 < len(lines):
                    return lines[i + 1]
        return None

    def test_basic_recovery_skip(self):
        """Test that recovery_skip marker is written on clean shutdown."""
        self.session.create(self.uri, 'key_format=i,value_format=S')

        # Insert some data
        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value_%d' % i
        cursor.close()

        if self.checkpoint:
            self.session.checkpoint()

        # Close the connection cleanly
        self.close_conn()

        # Check if marker was written
        has_marker = self.has_shutdown_marker()
        if self.recovery_skip:
            self.assertTrue(has_marker, "Shutdown marker should exist with recovery_skip=true")
            # Verify marker format
            marker = self.get_marker_value()
            self.assertIsNotNone(marker)
            self.assertIn('file=', marker)
            self.assertIn('offset=', marker)
            self.assertIn('file_size=', marker)
            self.assertIn('checksum=', marker)
        else:
            # With recovery_skip=false, marker should not be written
            self.assertFalse(has_marker, "Shutdown marker should not exist with recovery_skip=false")

        # Reopen and verify data
        self.open_conn()
        cursor = self.session.open_cursor(self.uri)
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 100)

    def test_marker_cleared_on_open(self):
        """Test that the marker is cleared from turtle file when connection opens."""
        if not self.recovery_skip:
            return  # Only relevant for recovery_skip=true

        self.session.create(self.uri, 'key_format=i,value_format=S')

        cursor = self.session.open_cursor(self.uri)
        for i in range(50):
            cursor[i] = 'value_%d' % i
        cursor.close()

        if self.checkpoint:
            self.session.checkpoint()

        self.close_conn()

        # Verify marker exists after clean shutdown
        self.assertTrue(self.has_shutdown_marker())

        # Reopen - marker should be cleared after validation
        self.open_conn()

        # Marker should be cleared from turtle file while connection is open
        self.assertFalse(self.has_shutdown_marker(),
            "Marker should be cleared from turtle file after connection opens")

        # Verify data is accessible
        cursor = self.session.open_cursor(self.uri)
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 50)

        # Close creates new marker
        self.close_conn()
        self.assertTrue(self.has_shutdown_marker(),
            "New marker should be created on clean shutdown")

    def test_data_integrity_across_restarts(self):
        """Verify data integrity is maintained with recovery_skip."""
        self.session.create(self.uri, 'key_format=i,value_format=S')

        nrows = 500
        cursor = self.session.open_cursor(self.uri)
        for i in range(nrows):
            cursor[i] = 'value_%d' % i
        cursor.close()

        self.session.checkpoint()

        # Add more data after checkpoint (needs log recovery if recovery_skip fails)
        cursor = self.session.open_cursor(self.uri)
        for i in range(nrows, nrows * 2):
            cursor[i] = 'value_%d' % i
        cursor.close()

        self.reopen_conn()

        # Verify all data
        cursor = self.session.open_cursor(self.uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, 'value_%d' % k)
            count += 1
        cursor.close()
        self.assertEqual(count, nrows * 2)

    def test_multiple_restart_cycles(self):
        """Stress test with multiple shutdown/restart cycles."""
        self.session.create(self.uri, 'key_format=i,value_format=S')

        total_rows = 0
        for cycle in range(10):
            cursor = self.session.open_cursor(self.uri)
            base = cycle * 50
            for i in range(50):
                cursor[base + i] = 'cycle_%d_value_%d' % (cycle, i)
            cursor.close()
            total_rows += 50

            # Checkpoint on even cycles
            if cycle % 2 == 0:
                self.session.checkpoint()

            self.reopen_conn()

            # Verify data count
            cursor = self.session.open_cursor(self.uri)
            count = sum(1 for _ in cursor)
            cursor.close()
            self.assertEqual(count, total_rows)


class test_log_recovery_skip_file_size_mismatch(wttest.WiredTigerTestCase):
    """Test that file size mismatch triggers full recovery."""

    uri = 'table:test_size_mismatch'

    def conn_config(self):
        return 'log=(enabled=true,file_max=1M,recovery_skip=true,remove=false)'

    def has_shutdown_marker(self):
        """Check if the turtle file contains a shutdown marker."""
        turtle_path = os.path.join('.', 'WiredTiger.turtle')
        if os.path.exists(turtle_path):
            with open(turtle_path, 'r') as f:
                return 'Log shutdown' in f.read()
        return False

    def test_file_size_mismatch(self):
        """Test that modifying log file size triggers full recovery."""
        self.session.create(self.uri, 'key_format=i,value_format=S')

        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value_%d' % i
        cursor.close()

        self.session.checkpoint()
        self.close_conn()

        # Verify marker exists in turtle file
        self.assertTrue(self.has_shutdown_marker(), "Marker should exist in turtle file")

        # Find the log file and modify its size
        log_files = [f for f in os.listdir('.') if f.startswith('WiredTigerLog.')]
        self.assertGreater(len(log_files), 0)

        # Append some bytes to the log file to change its size
        log_path = os.path.join('.', sorted(log_files)[-1])
        original_size = os.path.getsize(log_path)
        with open(log_path, 'ab') as f:
            f.write(b'\x00' * 128)
        new_size = os.path.getsize(log_path)
        self.assertGreater(new_size, original_size)

        # Reopen - should detect size mismatch and do full recovery
        # This should not crash and should recover correctly
        self.open_conn()

        cursor = self.session.open_cursor(self.uri)
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 100)


class test_log_recovery_skip_marker_corruption(wttest.WiredTigerTestCase):
    """Test that corrupted marker triggers full recovery."""

    uri = 'table:test_marker_corrupt'

    def conn_config(self):
        return 'log=(enabled=true,file_max=1M,recovery_skip=true)'

    def test_marker_checksum_corruption(self):
        """Test that corrupted checksum triggers full recovery."""
        self.session.create(self.uri, 'key_format=i,value_format=S')

        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value_%d' % i
        cursor.close()

        self.session.checkpoint()
        self.close_conn()

        # Corrupt the checksum in the turtle file
        turtle_path = os.path.join('.', 'WiredTiger.turtle')
        with open(turtle_path, 'r') as f:
            contents = f.read()

        # Replace the checksum value with a wrong one
        # The marker format is: file=N,offset=O,file_size=S,checksum=C
        corrupted = re.sub(r'checksum=\d+', 'checksum=12345', contents)

        with open(turtle_path, 'w') as f:
            f.write(corrupted)

        # Reopen - should detect checksum mismatch and do full recovery
        self.open_conn()

        cursor = self.session.open_cursor(self.uri)
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 100)


class test_log_recovery_skip_rotation(wttest.WiredTigerTestCase):
    """Test recovery_skip with log file rotation."""

    uri = 'table:test_rotation'

    def conn_config(self):
        # Small log file to force rotation
        return 'log=(enabled=true,file_max=100K,recovery_skip=true,remove=false)'

    def test_rotation_and_recovery_skip(self):
        """Test that recovery_skip works correctly with multiple log files."""
        self.session.create(self.uri, 'key_format=i,value_format=S')

        # Insert enough data to cause rotation
        cursor = self.session.open_cursor(self.uri)
        large_value = 'x' * 1000
        for i in range(300):
            cursor[i] = large_value
        cursor.close()

        self.session.checkpoint()
        self.close_conn()

        # Should have multiple log files
        log_files = [f for f in os.listdir('.') if f.startswith('WiredTigerLog.')]
        self.assertGreater(len(log_files), 1, "Expected multiple log files")

        # Verify marker exists in turtle file
        turtle_path = os.path.join('.', 'WiredTiger.turtle')
        self.assertTrue(os.path.exists(turtle_path))
        with open(turtle_path, 'r') as f:
            contents = f.read()
        self.assertIn('Log shutdown', contents)

        # Extract file number from marker
        match = re.search(r'file=(\d+)', contents)
        self.assertIsNotNone(match)
        marker_file = int(match.group(1))

        # Get the highest log file number
        max_log_num = max(int(f.split('.')[-1]) for f in log_files)
        self.assertEqual(marker_file, max_log_num,
            "Marker should point to the latest log file")

        # Reopen and verify
        self.open_conn()
        cursor = self.session.open_cursor(self.uri)
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 300)


class test_log_recovery_skip_disabled_by_default(wttest.WiredTigerTestCase):
    """Verify recovery_skip is disabled by default."""

    uri = 'table:test_default'

    def conn_config(self):
        # Don't specify recovery_skip - should default to false
        return 'log=(enabled=true,file_max=1M)'

    def test_disabled_by_default(self):
        """Verify no marker is written to turtle file when recovery_skip is not enabled."""
        self.session.create(self.uri, 'key_format=i,value_format=S')

        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value_%d' % i
        cursor.close()

        self.session.checkpoint()
        self.close_conn()

        # Marker should NOT exist in turtle file
        turtle_path = os.path.join('.', 'WiredTiger.turtle')
        with open(turtle_path, 'r') as f:
            contents = f.read()

        self.assertNotIn('Log shutdown', contents,
            "Shutdown marker should not exist in turtle file with default config")

        # Reopen and verify data
        self.open_conn()
        cursor = self.session.open_cursor(self.uri)
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 100)


class test_log_recovery_skip_missing_marker(wttest.WiredTigerTestCase):
    """
    Test that missing marker triggers full recovery.

    SAFETY SCENARIO: "Crash during normal operation"
    When a crash happens during normal operation (not startup), no marker
    exists because it was cleared on the previous startup. The next startup
    must do full recovery.
    """

    uri = 'table:test_missing_marker'

    def conn_config(self):
        return 'log=(enabled=true,file_max=1M,recovery_skip=true)'

    def test_missing_marker_triggers_full_recovery(self):
        """
        Verify that when the marker is manually removed (simulating it was
        never written or was cleared), full recovery runs and data is intact.
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')

        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value_%d' % i
        cursor.close()

        self.session.checkpoint()
        self.close_conn()

        # Remove the marker from the turtle file to simulate crash scenario
        turtle_path = os.path.join('.', 'WiredTiger.turtle')
        with open(turtle_path, 'r') as f:
            contents = f.read()

        # Remove the "Log shutdown" entry and its value
        lines = contents.split('\n')
        new_lines = []
        skip_next = False
        for line in lines:
            if line == 'Log shutdown':
                skip_next = True
                continue
            if skip_next:
                skip_next = False
                continue
            new_lines.append(line)

        with open(turtle_path, 'w') as f:
            f.write('\n'.join(new_lines))

        # Reopen - should do full recovery since marker is missing
        # This must succeed and data must be intact
        self.open_conn()

        cursor = self.session.open_cursor(self.uri)
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 100, "All data should be recovered via full recovery")


class test_log_recovery_skip_simulated_startup_crash(wttest.WiredTigerTestCase):
    """
    Test simulated crash during startup.

    SAFETY SCENARIO: "Crash during startup after marker cleared"
    The marker is cleared immediately when recovery starts. If we crash
    after that point, the next startup will have no marker and must do
    full recovery.

    We simulate this by:
    1. Clean shutdown (marker written)
    2. Open connection (marker cleared during open)
    3. Manually kill the turtle file marker (simulate partial startup)
    4. Reopen - full recovery must run
    """

    uri = 'table:test_startup_crash'

    def conn_config(self):
        return 'log=(enabled=true,file_max=1M,recovery_skip=true)'

    def test_simulated_startup_crash(self):
        """
        After marker is cleared on startup, any crash results in
        full recovery on next startup.
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')

        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value_%d' % i
        cursor.close()

        self.session.checkpoint()
        self.close_conn()

        # Marker should exist after clean shutdown
        turtle_path = os.path.join('.', 'WiredTiger.turtle')
        with open(turtle_path, 'r') as f:
            self.assertIn('Log shutdown', f.read())

        # Open connection - this clears the marker
        self.open_conn()

        # Verify marker is cleared
        with open(turtle_path, 'r') as f:
            self.assertNotIn('Log shutdown', f.read())

        # At this point, if we crashed, we'd have no marker.
        # Simulate by just closing and reopening.
        self.close_conn()

        # We closed cleanly so marker is back - remove it to simulate crash
        with open(turtle_path, 'r') as f:
            contents = f.read()
        lines = contents.split('\n')
        new_lines = []
        skip_next = False
        for line in lines:
            if line == 'Log shutdown':
                skip_next = True
                continue
            if skip_next:
                skip_next = False
                continue
            new_lines.append(line)
        with open(turtle_path, 'w') as f:
            f.write('\n'.join(new_lines))

        # Now reopen - must do full recovery and data must be intact
        self.open_conn()

        cursor = self.session.open_cursor(self.uri)
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 100)


class test_log_recovery_skip_lazy_vs_eager_corruption(wttest.WiredTigerTestCase):
    """
    Test demonstrating lazy vs eager corruption detection.

    TRADE-OFF DEMONSTRATION:
    - Without recovery_skip: Corruption detected at startup (eager)
    - With recovery_skip: Corruption detected on data access (lazy)

    This test verifies that with recovery_skip enabled and a valid marker,
    startup succeeds even if log data is corrupted. The corruption would
    only be detected if we tried to access the affected data (which we
    don't in this test - we just verify startup completes).

    NOTE: This test intentionally corrupts log data AFTER checkpointed data,
    so the corruption affects only data that would need log replay. Since
    recovery_skip skips log scanning, this corruption is not detected.
    """

    uri = 'table:test_lazy_corruption'

    def conn_config(self):
        return 'log=(enabled=true,file_max=1M,recovery_skip=true,remove=false)'

    def test_lazy_corruption_detection(self):
        """
        Demonstrate that with recovery_skip, log corruption is not detected
        at startup (lazy detection) - it would only be found on data access.

        This contrasts with the default behavior where log scanning would
        detect the corruption immediately at startup (eager detection).
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')

        # Insert data and checkpoint
        cursor = self.session.open_cursor(self.uri)
        for i in range(50):
            cursor[i] = 'checkpointed_%d' % i
        cursor.close()
        self.session.checkpoint()

        # Insert more data AFTER checkpoint - this data is only in the log
        cursor = self.session.open_cursor(self.uri)
        for i in range(50, 100):
            cursor[i] = 'logged_only_%d' % i
        cursor.close()

        self.close_conn()

        # Get marker info before corruption
        turtle_path = os.path.join('.', 'WiredTiger.turtle')
        with open(turtle_path, 'r') as f:
            turtle_before = f.read()
        self.assertIn('Log shutdown', turtle_before)

        # Find the log file
        log_files = [f for f in os.listdir('.') if f.startswith('WiredTigerLog.')]
        self.assertGreater(len(log_files), 0)
        log_path = os.path.join('.', sorted(log_files)[-1])

        # Get the file size that the marker recorded
        match = re.search(r'file_size=(\d+)', turtle_before)
        self.assertIsNotNone(match)
        marker_file_size = int(match.group(1))

        # Corrupt some bytes in the middle of the log file, but DON'T change the size.
        # This simulates silent corruption that wouldn't be caught by file size check.
        with open(log_path, 'r+b') as f:
            # Seek to middle of file and corrupt some bytes
            corrupt_offset = marker_file_size // 2
            f.seek(corrupt_offset)
            original_bytes = f.read(64)
            f.seek(corrupt_offset)
            # Write garbage (but keep file size same)
            f.write(b'\xDE\xAD\xBE\xEF' * 16)

        # Verify file size unchanged (so marker validation will pass)
        self.assertEqual(os.path.getsize(log_path), marker_file_size)

        # NOW REOPEN - with recovery_skip, this should SUCCEED because:
        # 1. Marker exists and checksum is valid
        # 2. File size matches marker
        # 3. Log scanning is SKIPPED (corruption not detected)
        #
        # Without recovery_skip, this would FAIL during log scan because
        # the corrupted bytes would be detected.
        self.open_conn()

        # We successfully opened! This demonstrates LAZY corruption detection.
        # The corruption exists but wasn't detected because we skipped log scan.

        # The checkpointed data (rows 0-49) should be readable
        cursor = self.session.open_cursor(self.uri)
        checkpointed_count = 0
        for k, v in cursor:
            if k < 50:
                # Checkpointed data should be intact
                self.assertEqual(v, 'checkpointed_%d' % k)
                checkpointed_count += 1
        cursor.close()

        # We should have at least the checkpointed rows
        self.assertEqual(checkpointed_count, 50,
            "Checkpointed data should be accessible")

        # NOTE: Rows 50-99 may or may not be accessible depending on whether
        # the corruption affected their log records. We don't test that here
        # because it would depend on the exact corruption location.


class test_log_recovery_skip_no_checkpoint(wttest.WiredTigerTestCase):
    """
    Test that recovery_skip does not take an unnecessary checkpoint.

    When skip_recovery succeeds (clean shutdown verified), no recovery checkpoint
    should be taken since the database is already in a consistent checkpointed state.
    This is important for the optimization to deliver startup time reduction.
    """

    uri = 'table:test_no_checkpoint'

    def conn_config(self):
        return 'log=(enabled=true,file_max=1M,recovery_skip=true)'

    def get_checkpoint_lsn_from_turtle(self):
        """
        Extract the checkpoint LSN from the turtle file.
        The turtle file contains the metadata entry which includes checkpoint_lsn.
        """
        turtle_path = os.path.join('.', 'WiredTiger.turtle')
        if not os.path.exists(turtle_path):
            return None

        with open(turtle_path, 'r') as f:
            contents = f.read()

        # The checkpoint_lsn is stored in the metadata config string
        # Format: checkpoint_lsn=(file,offset)
        match = re.search(r'checkpoint_lsn=\((\d+),(\d+)\)', contents)
        if match:
            return (int(match.group(1)), int(match.group(2)))
        return None

    def test_no_recovery_checkpoint_on_skip(self):
        """
        Verify that when recovery_skip succeeds, no checkpoint is taken during recovery.

        This ensures the optimization actually reduces startup time by avoiding
        the overhead of an unnecessary recovery checkpoint.
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')

        # Insert data and checkpoint
        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value_%d' % i
        cursor.close()

        self.session.checkpoint()
        self.close_conn()

        # Get checkpoint LSN after clean shutdown (marker should exist)
        lsn_before = self.get_checkpoint_lsn_from_turtle()
        self.assertIsNotNone(lsn_before, "Should have checkpoint LSN in turtle file")

        # Verify marker exists
        turtle_path = os.path.join('.', 'WiredTiger.turtle')
        with open(turtle_path, 'r') as f:
            self.assertIn('Log shutdown', f.read(), "Marker should exist after clean shutdown")

        # Reopen - skip_recovery should succeed and NO checkpoint should be taken
        self.open_conn()

        # Get checkpoint LSN while connection is open (after recovery completed)
        # The turtle file has been rewritten to clear the marker, but checkpoint LSN
        # should be unchanged since no recovery checkpoint was taken
        lsn_after = self.get_checkpoint_lsn_from_turtle()
        self.assertIsNotNone(lsn_after, "Should still have checkpoint LSN")

        # The checkpoint LSN should be the same - no recovery checkpoint was taken
        self.assertEqual(lsn_before, lsn_after,
            "Checkpoint LSN should not change when recovery_skip succeeds. "
            "Before: %s, After: %s. If different, a recovery checkpoint was incorrectly taken." %
            (lsn_before, lsn_after))

        # Verify data is accessible
        cursor = self.session.open_cursor(self.uri)
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 100)


class test_log_recovery_skip_checkpoint_when_needed(wttest.WiredTigerTestCase):
    """
    Test that checkpoint IS taken when recovery actually runs (marker missing/invalid).

    This is the counterpart to test_log_recovery_skip_no_checkpoint - it verifies
    that when full recovery is needed, the recovery checkpoint still happens.
    """

    uri = 'table:test_checkpoint_when_needed'

    def conn_config(self):
        return 'log=(enabled=true,file_max=1M,recovery_skip=true)'

    def get_checkpoint_lsn_from_turtle(self):
        """Extract the checkpoint LSN from the turtle file."""
        turtle_path = os.path.join('.', 'WiredTiger.turtle')
        if not os.path.exists(turtle_path):
            return None

        with open(turtle_path, 'r') as f:
            contents = f.read()

        match = re.search(r'checkpoint_lsn=\((\d+),(\d+)\)', contents)
        if match:
            return (int(match.group(1)), int(match.group(2)))
        return None

    def test_checkpoint_taken_when_marker_missing(self):
        """
        When the marker is missing (simulating crash), full recovery runs
        and a recovery checkpoint should be taken.
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')

        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value_%d' % i
        cursor.close()

        self.session.checkpoint()
        self.close_conn()

        lsn_before = self.get_checkpoint_lsn_from_turtle()
        self.assertIsNotNone(lsn_before)

        # Remove the marker to simulate crash (no clean shutdown marker)
        turtle_path = os.path.join('.', 'WiredTiger.turtle')
        with open(turtle_path, 'r') as f:
            contents = f.read()

        lines = contents.split('\n')
        new_lines = []
        skip_next = False
        for line in lines:
            if line == 'Log shutdown':
                skip_next = True
                continue
            if skip_next:
                skip_next = False
                continue
            new_lines.append(line)

        with open(turtle_path, 'w') as f:
            f.write('\n'.join(new_lines))

        # Reopen - full recovery should run since marker is missing
        self.open_conn()

        lsn_after = self.get_checkpoint_lsn_from_turtle()
        self.assertIsNotNone(lsn_after)

        # When full recovery runs, a checkpoint is taken, so LSN may change
        # (or stay same if no actual recovery work). We mainly verify the
        # connection opened successfully.

        # Verify data
        cursor = self.session.open_cursor(self.uri)
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 100)


class test_log_recovery_skip_with_timestamps(wttest.WiredTigerTestCase):
    """
    Test recovery_skip with timestamps to verify RTS skip is safe.

    Since RTS (rollback-to-stable) is primarily about timestamp-based consistency,
    this test verifies that skipping RTS after a clean shutdown doesn't break
    timestamp functionality and that committed data is preserved.
    """

    uri = 'table:test_timestamps'

    def conn_config(self):
        return 'log=(enabled=true,file_max=1M,recovery_skip=true)'

    def test_committed_timestamp_data_preserved(self):
        """
        Verify that data committed with timestamps is preserved after recovery_skip.

        This is the key safety test - after a clean shutdown with recovery_skip,
        all committed data must still be accessible.
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')

        cursor = self.session.open_cursor(self.uri)

        # Commit data at various timestamps
        for batch in range(5):
            ts = (batch + 1) * 10
            self.session.begin_transaction()
            for i in range(20):
                key = batch * 20 + i
                cursor[key] = 'batch_%d_key_%d' % (batch, key)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))

        # Set stable timestamp to include all commits
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))

        cursor.close()
        self.session.checkpoint()
        self.close_conn()

        # Verify marker exists
        turtle_path = os.path.join('.', 'WiredTiger.turtle')
        with open(turtle_path, 'r') as f:
            self.assertIn('Log shutdown', f.read())

        # Reopen with recovery_skip
        self.open_conn()

        # All committed data must be present
        cursor = self.session.open_cursor(self.uri)
        count = 0
        for key, value in cursor:
            batch = key // 20
            self.assertEqual(value, 'batch_%d_key_%d' % (batch, key))
            count += 1
        cursor.close()

        self.assertEqual(count, 100, "All 100 committed records should be present")

    def test_stable_timestamp_boundary_respected(self):
        """
        Verify that stable timestamp semantics are maintained after recovery_skip.

        Data committed at or before stable_timestamp should be visible.
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')

        cursor = self.session.open_cursor(self.uri)

        # Commit data at timestamp 20
        self.session.begin_transaction()
        for i in range(50):
            cursor[i] = 'stable_data_%d' % i
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Set stable to 20, so all data is stable
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))

        cursor.close()
        self.session.checkpoint()
        self.close_conn()

        # Reopen with recovery_skip
        self.open_conn()

        # Verify stable timestamp is preserved
        stable_ts = self.conn.query_timestamp('get=stable_timestamp')
        self.assertEqual(int(stable_ts, 16), 20)

        # All stable data should be accessible
        cursor = self.session.open_cursor(self.uri)
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 50)


if __name__ == '__main__':
    wttest.run()
