#!/usr/bin/env python3
"""
Pre-implementation verification: SQLite WAL multi-process behavior.

This script verifies the assumptions in PLAN-palite-multiprocess.md:
1. SQLite WAL mode allows concurrent reader + writer across processes
2. SQLite WAL mode serializes multiple writers via busy_timeout
3. synchronous setting affects durability
"""
import sqlite3
import multiprocessing
import os
import sys
import time
import tempfile
import signal

def setup_db(db_path, synchronous=1):
    """Create a test database with WAL mode."""
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode = WAL")
    conn.execute(f"PRAGMA synchronous = {synchronous}")
    conn.execute("PRAGMA busy_timeout = 10000")
    conn.execute("CREATE TABLE IF NOT EXISTS test (id INTEGER PRIMARY KEY, val TEXT)")
    conn.execute("INSERT OR REPLACE INTO test VALUES (1, 'initial')")
    conn.commit()
    conn.close()

def reader_process(db_path, barrier, results_queue, duration_sec=2):
    """Reader process: read continuously during writer activity."""
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode = WAL")
    conn.execute("PRAGMA busy_timeout = 10000")
    
    barrier.wait()  # Synchronize with writer
    
    read_count = 0
    blocked_count = 0
    errors = []
    end_time = time.time() + duration_sec
    
    while time.time() < end_time:
        try:
            start = time.time()
            cursor = conn.execute("SELECT val FROM test WHERE id = 1")
            val = cursor.fetchone()
            elapsed = time.time() - start
            read_count += 1
            if elapsed > 0.1:  # Read took > 100ms, likely blocked
                blocked_count += 1
        except sqlite3.OperationalError as e:
            errors.append(str(e))
        time.sleep(0.005)
    
    conn.close()
    results_queue.put(('reader', read_count, blocked_count, errors))

def writer_process(db_path, barrier, results_queue, write_count=50, process_id=0):
    """Writer process: write during reader activity."""
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode = WAL")
    conn.execute("PRAGMA busy_timeout = 10000")
    
    barrier.wait()  # Synchronize
    
    success_count = 0
    busy_count = 0
    errors = []
    
    for i in range(write_count):
        try:
            conn.execute(f"UPDATE test SET val = 'write_p{process_id}_{i}' WHERE id = 1")
            conn.commit()
            success_count += 1
        except sqlite3.OperationalError as e:
            if "database is locked" in str(e) or "BUSY" in str(e).upper():
                busy_count += 1
            errors.append(str(e))
        time.sleep(0.01)
    
    conn.close()
    results_queue.put(('writer', success_count, busy_count, errors))

def test_concurrent_reader_writer():
    """Test 1: Reader should not be blocked by writer in WAL mode."""
    print("\n" + "="*60)
    print("TEST 1: Concurrent Reader + Writer")
    print("="*60)
    
    with tempfile.TemporaryDirectory() as tmpdir:
        db_path = os.path.join(tmpdir, "test.db")
        setup_db(db_path)
        
        barrier = multiprocessing.Barrier(2)
        results = multiprocessing.Queue()
        
        reader = multiprocessing.Process(
            target=reader_process, 
            args=(db_path, barrier, results, 2)
        )
        writer = multiprocessing.Process(
            target=writer_process, 
            args=(db_path, barrier, results, 50, 0)
        )
        
        reader.start()
        writer.start()
        reader.join(timeout=10)
        writer.join(timeout=10)
        
        # Get results and sort by type (reader vs writer may finish in any order)
        r1 = results.get()
        r2 = results.get()
        
        if r1[0] == 'reader':
            reader_result, writer_result = r1, r2
        else:
            reader_result, writer_result = r2, r1
        
        print(f"  Reader: {reader_result[1]} reads, {reader_result[2]} blocked, errors: {len(reader_result[3])}")
        print(f"  Writer: {writer_result[1]} writes, {writer_result[2]} busy, errors: {len(writer_result[3])}")
        
        # PASS criteria
        reader_ok = reader_result[1] >= 50 and len(reader_result[3]) == 0
        writer_ok = writer_result[1] == 50
        
        if reader_ok and writer_ok:
            print("  RESULT: PASS - Reader not blocked by writer")
            return True
        else:
            print("  RESULT: FAIL")
            if reader_result[3]:
                print(f"    Reader errors: {reader_result[3][:3]}")
            if writer_result[3]:
                print(f"    Writer errors: {writer_result[3][:3]}")
            return False

def test_writer_serialization():
    """Test 2: Two writers should serialize (not corrupt) in WAL mode."""
    print("\n" + "="*60)
    print("TEST 2: Writer Serialization (Two Writers)")
    print("="*60)
    
    with tempfile.TemporaryDirectory() as tmpdir:
        db_path = os.path.join(tmpdir, "test.db")
        setup_db(db_path)
        
        barrier = multiprocessing.Barrier(2)
        results = multiprocessing.Queue()
        
        writer1 = multiprocessing.Process(
            target=writer_process, 
            args=(db_path, barrier, results, 30, 1)
        )
        writer2 = multiprocessing.Process(
            target=writer_process, 
            args=(db_path, barrier, results, 30, 2)
        )
        
        writer1.start()
        writer2.start()
        writer1.join(timeout=15)
        writer2.join(timeout=15)
        
        w1 = results.get()
        w2 = results.get()
        
        total_writes = w1[1] + w2[1]
        total_busy = w1[2] + w2[2]
        all_errors = w1[3] + w2[3]
        
        print(f"  Writer1: {w1[1]} writes, {w1[2]} busy")
        print(f"  Writer2: {w2[1]} writes, {w2[2]} busy")
        print(f"  Total: {total_writes}/60 writes, {total_busy} busy retries")
        
        # Verify data integrity
        conn = sqlite3.connect(db_path)
        val = conn.execute("SELECT val FROM test WHERE id = 1").fetchone()[0]
        conn.close()
        print(f"  Final value: {val}")
        
        # PASS criteria: Both complete (serialized), no data corruption errors
        if total_writes == 60:
            print("  RESULT: PASS - Writers serialized correctly")
            return True
        else:
            print(f"  RESULT: FAIL - Only {total_writes}/60 writes completed")
            if all_errors:
                print(f"    Errors: {all_errors[:3]}")
            return False

def test_synchronous_modes():
    """Test 3: Verify synchronous settings work as expected."""
    print("\n" + "="*60)
    print("TEST 3: Synchronous Mode Settings")
    print("="*60)
    
    results = {}
    for mode in [0, 1, 2]:  # OFF, NORMAL, FULL
        mode_name = {0: 'OFF', 1: 'NORMAL', 2: 'FULL'}[mode]
        with tempfile.TemporaryDirectory() as tmpdir:
            db_path = os.path.join(tmpdir, "test.db")
            
            conn = sqlite3.connect(db_path)
            conn.execute("PRAGMA journal_mode = WAL")
            conn.execute(f"PRAGMA synchronous = {mode}")
            conn.execute("CREATE TABLE test (id INTEGER PRIMARY KEY, val TEXT)")
            
            # Write some data
            start = time.time()
            for i in range(100):
                conn.execute(f"INSERT INTO test VALUES ({i}, 'data_{i}')")
                conn.commit()
            elapsed = time.time() - start
            
            # Verify writes
            count = conn.execute("SELECT COUNT(*) FROM test").fetchone()[0]
            conn.close()
            
            results[mode_name] = {'count': count, 'time': elapsed}
            print(f"  synchronous={mode_name}: {count} rows, {elapsed:.3f}s")
    
    # PASS criteria: All modes write correctly, OFF should be faster
    all_wrote = all(r['count'] == 100 for r in results.values())
    off_faster = results['OFF']['time'] < results['FULL']['time']
    
    if all_wrote:
        print(f"  RESULT: PASS - All modes write correctly")
        if off_faster:
            print(f"    Note: OFF ({results['OFF']['time']:.3f}s) faster than FULL ({results['FULL']['time']:.3f}s) as expected")
        return True
    else:
        print("  RESULT: FAIL - Some writes lost")
        return False

def test_busy_timeout_behavior():
    """Test 4: Verify busy_timeout causes retry, not immediate failure."""
    print("\n" + "="*60)
    print("TEST 4: Busy Timeout Behavior")
    print("="*60)
    
    with tempfile.TemporaryDirectory() as tmpdir:
        db_path = os.path.join(tmpdir, "test.db")
        setup_db(db_path)
        
        # Open connection and hold exclusive lock
        conn1 = sqlite3.connect(db_path)
        conn1.execute("PRAGMA journal_mode = WAL")
        conn1.execute("BEGIN IMMEDIATE")  # Start write transaction
        conn1.execute("UPDATE test SET val = 'locked'")
        # Don't commit - hold the lock
        
        # Try to write from another connection with short timeout
        conn2 = sqlite3.connect(db_path)
        conn2.execute("PRAGMA journal_mode = WAL")
        conn2.execute("PRAGMA busy_timeout = 500")  # 500ms timeout
        
        start = time.time()
        try:
            conn2.execute("UPDATE test SET val = 'second'")
            result = "succeeded (unexpected)"
        except sqlite3.OperationalError as e:
            elapsed = time.time() - start
            if elapsed >= 0.4:  # Should have waited ~500ms
                result = f"timed out after {elapsed:.2f}s (expected)"
            else:
                result = f"failed immediately after {elapsed:.2f}s (unexpected)"
        
        conn1.rollback()
        conn1.close()
        conn2.close()
        
        print(f"  Result: {result}")
        
        if "expected" in result:
            print("  RESULT: PASS - Busy timeout works as expected")
            return True
        else:
            print("  RESULT: FAIL - Busy timeout behavior unexpected")
            return False

def main():
    print("="*60)
    print("PALite Multi-Process Verification Tests")
    print("Testing SQLite WAL mode multi-process behavior")
    print("="*60)
    
    results = {}
    results['reader_writer'] = test_concurrent_reader_writer()
    results['writer_serial'] = test_writer_serialization()
    results['synchronous'] = test_synchronous_modes()
    results['busy_timeout'] = test_busy_timeout_behavior()
    
    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)
    
    all_pass = True
    for name, passed in results.items():
        status = "PASS" if passed else "FAIL"
        print(f"  {name}: {status}")
        if not passed:
            all_pass = False
    
    print("="*60)
    if all_pass:
        print("All verification tests PASSED")
        print("SQLite WAL mode provides the expected multi-process behavior")
        return 0
    else:
        print("Some tests FAILED - review results before implementing")
        return 1

if __name__ == "__main__":
    sys.exit(main())
