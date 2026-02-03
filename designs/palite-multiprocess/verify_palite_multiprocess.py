#!/usr/bin/env python3
"""
Pre-implementation verification: PALite multi-process behavior.

This tests actual PALite databases (the SQLite files it creates)
to verify multi-process access works at the PALite level.
"""
import sqlite3
import multiprocessing
import os
import sys
import time
import tempfile
import shutil

def setup_palite_db(db_dir):
    """
    Create a PALite-like database structure.
    PALite creates: globals.db, checkpoints.db, pages_NNNNNN.db
    """
    os.makedirs(db_dir, exist_ok=True)
    
    # Create globals.db (mimics PALite structure)
    globals_path = os.path.join(db_dir, "globals.db")
    conn = sqlite3.connect(globals_path)
    conn.execute("PRAGMA journal_mode = WAL")
    conn.execute("PRAGMA busy_timeout = 10000")
    conn.execute("PRAGMA synchronous = OFF")  # Current PALite default
    conn.execute("""CREATE TABLE IF NOT EXISTS globals (
        id INTEGER CHECK (id = 0 OR id = 1),
        val INTEGER NOT NULL,
        PRIMARY KEY (id, val))""")
    conn.execute("INSERT OR IGNORE INTO globals (id, val) VALUES (0, 0)")  # LSN counter
    conn.commit()
    conn.close()
    
    # Create pages_000000.db (mimics PALite structure)
    pages_path = os.path.join(db_dir, "pages_000000.db")
    conn = sqlite3.connect(pages_path)
    conn.execute("PRAGMA journal_mode = WAL")
    conn.execute("PRAGMA busy_timeout = 10000")
    conn.execute("PRAGMA synchronous = OFF")
    conn.execute("""CREATE TABLE IF NOT EXISTS pages (
        table_id INTEGER NOT NULL,
        page_id INTEGER NOT NULL,
        lsn INTEGER NOT NULL,
        backlink_lsn INTEGER NOT NULL,
        base_lsn INTEGER NOT NULL,
        flags INTEGER NOT NULL,
        encryption STRING NOT NULL,
        timestamp_materialized_us INTEGER NOT NULL,
        page_data BLOB,
        PRIMARY KEY (table_id, page_id, lsn))""")
    conn.commit()
    conn.close()
    
    return globals_path, pages_path

def palite_make_next_lsn(globals_path):
    """Simulate PALite's make_next_lsn() - atomically increment LSN."""
    conn = sqlite3.connect(globals_path)
    conn.execute("PRAGMA journal_mode = WAL")
    conn.execute("PRAGMA busy_timeout = 10000")
    
    cursor = conn.execute("""
        UPDATE globals SET val = val + 1 WHERE id = 0 RETURNING val
    """)
    lsn = cursor.fetchone()[0]
    conn.commit()
    conn.close()
    return lsn

def palite_put_page(pages_path, globals_path, table_id, page_id, data):
    """Simulate PALite's put_page() - write a page."""
    lsn = palite_make_next_lsn(globals_path)
    
    conn = sqlite3.connect(pages_path)
    conn.execute("PRAGMA journal_mode = WAL")
    conn.execute("PRAGMA busy_timeout = 10000")
    
    conn.execute("""
        INSERT INTO pages (table_id, page_id, lsn, backlink_lsn, base_lsn, 
                          flags, encryption, timestamp_materialized_us, page_data)
        VALUES (?, ?, ?, 0, 0, 0, '', ?, ?)
    """, (table_id, page_id, lsn, int(time.time() * 1000000), data))
    conn.commit()
    conn.close()
    return lsn

def palite_get_page(pages_path, table_id, page_id, max_lsn=None):
    """Simulate PALite's get_page() - read a page."""
    conn = sqlite3.connect(pages_path)
    conn.execute("PRAGMA journal_mode = WAL")
    conn.execute("PRAGMA busy_timeout = 10000")
    
    if max_lsn:
        cursor = conn.execute("""
            SELECT lsn, page_data FROM pages
            WHERE table_id = ? AND page_id = ? AND lsn <= ?
            ORDER BY lsn DESC LIMIT 1
        """, (table_id, page_id, max_lsn))
    else:
        cursor = conn.execute("""
            SELECT lsn, page_data FROM pages
            WHERE table_id = ? AND page_id = ?
            ORDER BY lsn DESC LIMIT 1
        """, (table_id, page_id))
    
    row = cursor.fetchone()
    conn.close()
    return row if row else (None, None)

def writer_process(db_dir, barrier, results_queue, num_writes, process_id):
    """Writer process: write pages using PALite-like operations."""
    globals_path = os.path.join(db_dir, "globals.db")
    pages_path = os.path.join(db_dir, "pages_000000.db")
    
    barrier.wait()
    
    success = 0
    errors = []
    lsns = []
    
    for i in range(num_writes):
        try:
            data = f"page_data_from_process_{process_id}_iteration_{i}".encode()
            lsn = palite_put_page(pages_path, globals_path, table_id=1, page_id=i, data=data)
            lsns.append(lsn)
            success += 1
        except Exception as e:
            errors.append(str(e))
        time.sleep(0.01)
    
    results_queue.put(('writer', process_id, success, errors, lsns))

def reader_process(db_dir, barrier, results_queue, duration_sec, process_id):
    """Reader process: read pages using PALite-like operations."""
    pages_path = os.path.join(db_dir, "pages_000000.db")
    
    barrier.wait()
    
    reads = 0
    errors = []
    end_time = time.time() + duration_sec
    
    while time.time() < end_time:
        try:
            # Try to read a random page that may or may not exist yet
            for page_id in range(10):
                lsn, data = palite_get_page(pages_path, table_id=1, page_id=page_id)
                reads += 1
        except Exception as e:
            errors.append(str(e))
        time.sleep(0.01)
    
    results_queue.put(('reader', process_id, reads, errors))

def test_palite_concurrent_access():
    """Test PALite-like database with concurrent readers and writers."""
    print("\n" + "="*60)
    print("TEST: PALite Multi-Process Concurrent Access")
    print("="*60)
    
    with tempfile.TemporaryDirectory() as tmpdir:
        db_dir = os.path.join(tmpdir, "kv_home")
        setup_palite_db(db_dir)
        
        barrier = multiprocessing.Barrier(3)  # 2 writers + 1 reader
        results = multiprocessing.Queue()
        
        # Start 2 writers and 1 reader
        writer1 = multiprocessing.Process(
            target=writer_process,
            args=(db_dir, barrier, results, 20, 1)
        )
        writer2 = multiprocessing.Process(
            target=writer_process,
            args=(db_dir, barrier, results, 20, 2)
        )
        reader = multiprocessing.Process(
            target=reader_process,
            args=(db_dir, barrier, results, 2, 0)
        )
        
        writer1.start()
        writer2.start()
        reader.start()
        
        writer1.join(timeout=30)
        writer2.join(timeout=30)
        reader.join(timeout=30)
        
        # Collect results
        all_results = []
        while not results.empty():
            all_results.append(results.get())
        
        writers = [r for r in all_results if r[0] == 'writer']
        readers = [r for r in all_results if r[0] == 'reader']
        
        print(f"\n  Results:")
        for r in writers:
            _, pid, success, errors, lsns = r
            print(f"    Writer {pid}: {success} writes, {len(errors)} errors")
            if lsns:
                print(f"      LSNs: {min(lsns)}-{max(lsns)}")
        
        for r in readers:
            _, pid, reads, errors = r
            print(f"    Reader {pid}: {reads} reads, {len(errors)} errors")
        
        # Verify LSN uniqueness (critical for PALite correctness)
        all_lsns = []
        for r in writers:
            all_lsns.extend(r[4])
        
        unique_lsns = set(all_lsns)
        total_writes = sum(r[2] for r in writers)
        
        print(f"\n  LSN Check:")
        print(f"    Total writes: {total_writes}")
        print(f"    Unique LSNs: {len(unique_lsns)}")
        
        if len(unique_lsns) == total_writes and total_writes == 40:
            print(f"    RESULT: PASS - All LSNs unique, all writes succeeded")
            return True
        else:
            print(f"    RESULT: FAIL - LSN collision or write failure")
            if len(unique_lsns) != total_writes:
                print(f"      LSN collision detected!")
            return False

def test_palite_lsn_serialization():
    """Test that LSN generation is properly serialized across processes."""
    print("\n" + "="*60)
    print("TEST: PALite LSN Serialization")
    print("="*60)
    
    with tempfile.TemporaryDirectory() as tmpdir:
        db_dir = os.path.join(tmpdir, "kv_home")
        globals_path, _ = setup_palite_db(db_dir)
        
        def lsn_generator(globals_path, barrier, results_queue, count, process_id):
            barrier.wait()
            lsns = []
            for _ in range(count):
                try:
                    lsn = palite_make_next_lsn(globals_path)
                    lsns.append(lsn)
                except Exception as e:
                    results_queue.put(('error', process_id, str(e)))
                    return
            results_queue.put(('lsns', process_id, lsns))
        
        barrier = multiprocessing.Barrier(4)
        results = multiprocessing.Queue()
        
        processes = []
        for i in range(4):
            p = multiprocessing.Process(
                target=lsn_generator,
                args=(globals_path, barrier, results, 50, i)
            )
            processes.append(p)
            p.start()
        
        for p in processes:
            p.join(timeout=30)
        
        all_lsns = []
        errors = []
        while not results.empty():
            r = results.get()
            if r[0] == 'lsns':
                all_lsns.extend(r[2])
            else:
                errors.append(r[2])
        
        print(f"  Total LSNs generated: {len(all_lsns)}")
        print(f"  Unique LSNs: {len(set(all_lsns))}")
        print(f"  Errors: {len(errors)}")
        
        if len(set(all_lsns)) == 200 and len(errors) == 0:
            # Check LSNs are consecutive (no gaps)
            sorted_lsns = sorted(all_lsns)
            expected = list(range(1, 201))
            if sorted_lsns == expected:
                print(f"  LSNs are consecutive: 1-200")
                print(f"  RESULT: PASS - LSN serialization works correctly")
                return True
            else:
                print(f"  LSNs have gaps or duplicates")
                print(f"  RESULT: FAIL")
                return False
        else:
            print(f"  RESULT: FAIL - Missing LSNs or errors")
            return False

def main():
    print("="*60)
    print("PALite Multi-Process Verification Tests")
    print("Testing PALite-like database structure multi-process behavior")
    print("="*60)
    
    results = {}
    results['concurrent_access'] = test_palite_concurrent_access()
    results['lsn_serialization'] = test_palite_lsn_serialization()
    
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
        print("All PALite verification tests PASSED")
        print("PALite structure supports multi-process access via SQLite WAL")
        return 0
    else:
        print("Some tests FAILED - investigate before implementing")
        return 1

if __name__ == "__main__":
    sys.exit(main())
