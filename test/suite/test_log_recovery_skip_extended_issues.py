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

# test_log_recovery_skip_extended_issues.py
#
# Tests for specific edge cases in the extended marker format for recovery_skip.
# These tests verify correctness of:
#   1. base_write_gen handling across multiple restart cycles
#   2. Proper functioning after many recovery_skip cycles
#   3. History store handling consistency


class test_log_recovery_skip_base_write_gen(wttest.WiredTigerTestCase):
    """
    Test that base_write_gen is correctly maintained across recovery_skip restarts.
    
    The base_write_gen is critical for page obsolescence checking and should be
    set to the MAX write_gen + 1 across all files. When skipping the metadata scan,
    we need to ensure this is still correctly initialized.
    """

    def conn_config(self):
        return 'log=(enabled=true,file_max=1M,recovery_skip=true)'

    def test_multiple_restart_cycles_write_gen(self):
        """
        Test that write_gen is correctly maintained across multiple recovery_skip
        restart cycles with modifications between each cycle.
        
        This test verifies that skipping metadata scan doesn't cause write_gen
        issues that would manifest over multiple cycles.
        """
        # Create multiple tables
        for i in range(5):
            self.session.create(f'table:test{i}', 'key_format=i,value_format=S')
        
        # Do multiple restart cycles with writes between each
        for cycle in range(5):
            # Write different amounts to different tables to create varying write_gens
            for i in range(5):
                cursor = self.session.open_cursor(f'table:test{i}')
                # Table 0 gets most writes, table 4 gets fewest
                for j in range((5 - i) * 100):
                    cursor[cycle * 1000 + j] = f'value_{cycle}_{j}'
                cursor.close()
            
            self.session.checkpoint()
            self.close_conn()
            
            # Reopen with recovery_skip
            self.open_conn()
        
        # Verify all data is intact
        total = 0
        for i in range(5):
            cursor = self.session.open_cursor(f'table:test{i}')
            count = sum(1 for _ in cursor)
            cursor.close()
            # Each table should have (5-i)*100 * 5 cycles = (5-i)*500 rows
            expected = (5 - i) * 100 * 5
            self.assertEqual(count, expected, 
                f"Table test{i} should have {expected} rows, got {count}")
            total += count
        
        self.assertEqual(total, 7500)  # 500+400+300+200+100 = 1500 * 5 = 7500

    def test_create_modify_restart_cycle(self):
        """
        Test creating tables, modifying them heavily, then doing recovery_skip
        restart cycles. This exercises the base_write_gen being correctly
        restored from the persisted value.
        """
        # First cycle: create and heavily modify
        self.session.create('table:heavy', 'key_format=i,value_format=S')
        cursor = self.session.open_cursor('table:heavy')
        for i in range(1000):
            cursor[i] = 'x' * 100
        cursor.close()
        
        # Modify same keys multiple times to increase write_gen
        for _ in range(5):
            cursor = self.session.open_cursor('table:heavy')
            for i in range(1000):
                cursor[i] = 'y' * 100
            cursor.close()
            self.session.checkpoint()
        
        self.close_conn()
        
        # Multiple recovery_skip restarts
        for cycle in range(3):
            self.open_conn()
            
            # Create a new table each cycle
            self.session.create(f'table:new{cycle}', 'key_format=i,value_format=S')
            cursor = self.session.open_cursor(f'table:new{cycle}')
            for i in range(100):
                cursor[i] = f'new_value_{cycle}'
            cursor.close()
            
            self.session.checkpoint()
            self.close_conn()
        
        # Final verification
        self.open_conn()
        
        # Check heavy table
        cursor = self.session.open_cursor('table:heavy')
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 1000)
        
        # Check all new tables
        for cycle in range(3):
            cursor = self.session.open_cursor(f'table:new{cycle}')
            count = sum(1 for _ in cursor)
            cursor.close()
            self.assertEqual(count, 100)


class test_log_recovery_skip_hs_consistency(wttest.WiredTigerTestCase):
    """
    Test history store handling consistency with recovery_skip.
    
    Verify that when using the extended marker with cached hs_exists,
    the history store is configured correctly.
    """

    def conn_config(self):
        return 'log=(enabled=true,file_max=1M,recovery_skip=true)'

    def get_marker_hs_exists(self):
        """Get hs_exists value from the shutdown marker."""
        turtle_path = os.path.join('.', 'WiredTiger.turtle')
        if not os.path.exists(turtle_path):
            return None
        with open(turtle_path, 'r') as f:
            contents = f.read()
        match = re.search(r'hs_exists=(\d+)', contents)
        if match:
            return int(match.group(1))
        return None

    def test_hs_operations_after_recovery_skip(self):
        """
        Test that history store operations work correctly after recovery_skip.
        
        This exercises the history store configuration path.
        """
        # Create a table and do operations that may involve HS
        self.session.create('table:hs_test', 'key_format=i,value_format=S')
        
        # Insert data
        cursor = self.session.open_cursor('table:hs_test')
        for i in range(500):
            cursor[i] = 'initial_value'
        cursor.close()
        
        self.session.checkpoint()
        
        # Update data (may create history store entries)
        cursor = self.session.open_cursor('table:hs_test')
        for i in range(500):
            cursor[i] = 'updated_value'
        cursor.close()
        
        self.session.checkpoint()
        self.close_conn()
        
        # Reopen with recovery_skip
        self.open_conn()
        
        # Verify data
        cursor = self.session.open_cursor('table:hs_test')
        count = 0
        for key, value in cursor:
            count += 1
            self.assertEqual(value, 'updated_value')
        cursor.close()
        self.assertEqual(count, 500)
        
        # Do more updates
        cursor = self.session.open_cursor('table:hs_test')
        for i in range(500):
            cursor[i] = 'third_value'
        cursor.close()
        
        self.session.checkpoint()
        self.close_conn()
        
        # Another recovery_skip restart
        self.open_conn()
        
        cursor = self.session.open_cursor('table:hs_test')
        count = 0
        for key, value in cursor:
            count += 1
            self.assertEqual(value, 'third_value')
        cursor.close()
        self.assertEqual(count, 500)


class test_log_recovery_skip_stress(wttest.WiredTigerTestCase):
    """
    Stress test for recovery_skip with many tables and restart cycles.
    """

    def conn_config(self):
        return 'log=(enabled=true,file_max=1M,recovery_skip=true)'

    def test_many_tables_many_restarts(self):
        """
        Create many tables, do many restart cycles, verify everything works.
        
        This stress tests the max_fileid caching across many cycles.
        """
        num_tables = 20
        num_cycles = 5
        
        # Initial creation
        for i in range(num_tables):
            self.session.create(f'table:stress{i}', 'key_format=i,value_format=S')
            cursor = self.session.open_cursor(f'table:stress{i}')
            cursor[0] = f'initial_{i}'
            cursor.close()
        
        self.session.checkpoint()
        self.close_conn()
        
        # Multiple restart cycles, adding tables each time
        for cycle in range(num_cycles):
            self.open_conn()
            
            # Verify existing tables
            for i in range(num_tables + cycle):
                cursor = self.session.open_cursor(f'table:stress{i}')
                count = sum(1 for _ in cursor)
                cursor.close()
                self.assertGreater(count, 0, f"Table stress{i} should have data")
            
            # Create a new table
            new_table = f'table:stress{num_tables + cycle}'
            self.session.create(new_table, 'key_format=i,value_format=S')
            cursor = self.session.open_cursor(new_table)
            cursor[0] = f'cycle_{cycle}'
            cursor.close()
            
            self.session.checkpoint()
            self.close_conn()
        
        # Final verification
        self.open_conn()
        
        total_tables = num_tables + num_cycles
        for i in range(total_tables):
            cursor = self.session.open_cursor(f'table:stress{i}')
            count = sum(1 for _ in cursor)
            cursor.close()
            self.assertGreater(count, 0, f"Table stress{i} should have data after all cycles")
        
        # Create one more table to verify file ID allocation still works
        self.session.create('table:final', 'key_format=i,value_format=S')
        cursor = self.session.open_cursor('table:final')
        cursor[0] = 'final_value'
        cursor.close()
        
        self.session.checkpoint()
        self.close_conn()
        
        self.open_conn()
        cursor = self.session.open_cursor('table:final')
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 1)


class test_log_recovery_skip_alternating_modes(wttest.WiredTigerTestCase):
    """
    Test alternating between recovery_skip and normal recovery.
    
    This verifies that:
    1. Normal recovery correctly handles state from recovery_skip
    2. recovery_skip correctly handles state from normal recovery
    """

    def conn_config(self):
        return 'log=(enabled=true,file_max=1M,recovery_skip=true)'

    def reopen_without_skip(self):
        """Reopen connection without recovery_skip."""
        self.conn = self.wiredtiger_open('.', 'log=(enabled=true,file_max=1M)')
        self.session = self.conn.open_session()

    def test_alternate_skip_and_full_recovery(self):
        """
        Alternate between recovery_skip and full recovery to verify
        consistency is maintained.
        """
        self.session.create('table:alternate', 'key_format=i,value_format=S')
        
        for cycle in range(6):
            # Write data
            cursor = self.session.open_cursor('table:alternate')
            for i in range(100):
                cursor[cycle * 100 + i] = f'value_{cycle}_{i}'
            cursor.close()
            
            self.session.checkpoint()
            self.close_conn()
            
            # Alternate between recovery modes
            if cycle % 2 == 0:
                self.open_conn()  # Uses recovery_skip=true
            else:
                self.reopen_without_skip()  # Full recovery
        
        # Final check
        cursor = self.session.open_cursor('table:alternate')
        count = sum(1 for _ in cursor)
        cursor.close()
        self.assertEqual(count, 600)  # 6 cycles * 100 rows


if __name__ == '__main__':
    wttest.run()
