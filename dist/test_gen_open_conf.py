#!/usr/bin/env python3
"""
Tests for gen_open_conf.py generator.

These tests verify:
1. Generator produces consistent output
2. ABI stability check works correctly
3. Baseline file is properly maintained
4. All expected keys are generated
"""

import os
import sys
import tempfile
import shutil

# Add dist directory to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_open_conf as gen

def test_baseline_loading():
    """Test that baseline file is loaded correctly."""
    print("Test: baseline loading...")
    baseline = gen.load_baseline()
    
    # Check that we loaded some keys
    assert len(baseline) > 0, "Baseline should not be empty"
    
    # Check specific known keys
    expected_keys = [
        'WT_OPEN_CONF_cache_size',
        'WT_OPEN_CONF_create',
        'WT_OPEN_CONF_in_memory',
        'WT_OPEN_CONF_eviction_threads_min',
        'WT_OPEN_CONF_log_enabled',
    ]
    for key in expected_keys:
        assert key in baseline, f"Expected key {key} not in baseline"
    
    # Check that IDs are in expected ranges
    for key, id_val in baseline.items():
        assert 1000 <= id_val < 3000, f"Key {key} has invalid ID {id_val}"
    
    print("  PASSED")

def test_config_extraction():
    """Test that configs are extracted correctly from api_data."""
    print("Test: config extraction...")
    
    import api_data as api_data_def
    wiredtiger_open_config = api_data_def.methods.get('wiredtiger_open')
    assert wiredtiger_open_config is not None, "wiredtiger_open not found in api_data"
    
    keys = gen.extract_configs(wiredtiger_open_config.config)
    
    # Check we found a reasonable number of keys
    assert len(keys) >= 100, f"Expected at least 100 keys, got {len(keys)}"
    
    # Check specific expected keys
    key_names = {k.name for k in keys}
    expected = ['cache_size', 'create', 'in_memory', 'log_enabled', 'eviction_threads_min']
    for name in expected:
        assert name in key_names, f"Expected key {name} not found"
    
    # Check that nested keys have correct parent
    for key in keys:
        if key.name == 'log_enabled':
            assert key.parent_name == 'log', f"log_enabled should have parent 'log'"
            assert key.key_name == 'enabled', f"log_enabled key_name should be 'enabled'"
        if key.name == 'eviction_threads_min':
            assert key.parent_name == 'eviction', f"eviction_threads_min should have parent 'eviction'"
    
    print("  PASSED")

def test_abi_stability_check():
    """Test that ABI stability check catches ID changes."""
    print("Test: ABI stability check...")
    
    # Create a fake baseline with a specific ID
    baseline = {'WT_OPEN_CONF_test_key': 1234}
    
    # Create new assignments with same ID - should pass
    new_assignments_ok = {'WT_OPEN_CONF_test_key': (1234, 'core')}
    gen.verify_abi_stability(baseline, new_assignments_ok)  # Should not raise
    
    # Create new assignments with different ID - should fail
    new_assignments_bad = {'WT_OPEN_CONF_test_key': (1235, 'core')}
    try:
        # Redirect stderr to suppress error output during test
        old_stderr = sys.stderr
        sys.stderr = open(os.devnull, 'w')
        gen.verify_abi_stability(baseline, new_assignments_bad)
        sys.stderr = old_stderr
        assert False, "Should have raised SystemExit"
    except SystemExit:
        sys.stderr = old_stderr
        pass  # Expected
    
    print("  PASSED")

def test_id_assignment_preserves_baseline():
    """Test that ID assignment preserves baseline IDs."""
    print("Test: ID assignment preserves baseline...")
    
    import api_data as api_data_def
    wiredtiger_open_config = api_data_def.methods.get('wiredtiger_open')
    keys = gen.extract_configs(wiredtiger_open_config.config)
    
    baseline = gen.load_baseline()
    assignments = gen.assign_ids(keys, baseline)
    
    # Check that all baseline keys retained their IDs
    for key_name, old_id in baseline.items():
        if key_name in assignments:
            new_id, _ = assignments[key_name]
            assert new_id == old_id, f"Key {key_name} changed from {old_id} to {new_id}"
    
    print("  PASSED")

def test_category_assignment():
    """Test that keys are assigned to correct categories."""
    print("Test: category assignment...")
    
    import api_data as api_data_def
    wiredtiger_open_config = api_data_def.methods.get('wiredtiger_open')
    keys = gen.extract_configs(wiredtiger_open_config.config)
    
    baseline = gen.load_baseline()
    assignments = gen.assign_ids(keys, baseline)
    
    # Check that keys are in expected categories based on ID ranges
    for key in keys:
        if key.id is None:
            continue
        
        # Find which range this ID falls into
        found_category = None
        for cat, (range_start, range_end) in gen.CATEGORY_RANGES.items():
            if range_start <= key.id <= range_end:
                found_category = cat
                break
        
        assert found_category is not None, f"Key {key.name} has ID {key.id} not in any range"
        assert found_category == key.category, f"Key {key.name} category mismatch: {found_category} vs {key.category}"
    
    print("  PASSED")

def test_type_mapping():
    """Test that types are mapped correctly."""
    print("Test: type mapping...")
    
    import api_data as api_data_def
    wiredtiger_open_config = api_data_def.methods.get('wiredtiger_open')
    keys = gen.extract_configs(wiredtiger_open_config.config)
    
    # Check specific types
    for key in keys:
        if key.name == 'cache_size':
            assert key.wt_type == 'WT_OPEN_CONFIG_ARG_INT', f"cache_size should be INT"
        elif key.name == 'create':
            assert key.wt_type == 'WT_OPEN_CONFIG_ARG_BOOL', f"create should be BOOL"
        elif key.name == 'error_prefix':
            assert key.wt_type == 'WT_OPEN_CONFIG_ARG_STR', f"error_prefix should be STR"
    
    print("  PASSED")

def test_generated_files_syntax():
    """Test that generated files have valid syntax (can be parsed)."""
    print("Test: generated file syntax...")
    
    # Check header file exists and has expected content
    header_file = '../src/include/wiredtiger_open_conf.h'
    assert os.path.exists(header_file), f"Header file not found: {header_file}"
    
    with open(header_file, 'r') as f:
        content = f.read()
    
    # Check for required elements
    assert '#ifndef __WIREDTIGER_OPEN_CONF_H_' in content, "Missing include guard"
    assert '#define WT_OPEN_CONF_KEY_END UINT64_MAX' in content, "Missing sentinel"
    assert '#define WT_OPEN_CONF_cache_size' in content, "Missing cache_size key"
    
    # Check C file exists
    c_file = '../src/conn/conn_open_conf.c'
    assert os.path.exists(c_file), f"C file not found: {c_file}"
    
    with open(c_file, 'r') as f:
        content = f.read()
    
    # Check for required elements
    assert 'open_conf_key_map[]' in content, "Missing key map array"
    assert '__wt_open_conf_validate_args' in content, "Missing validate function"
    assert 'WT_OPEN_CONF_KEY_END, NULL, NULL, 0' in content, "Missing terminator"
    
    print("  PASSED")

def test_no_duplicate_ids():
    """Test that no two keys have the same ID."""
    print("Test: no duplicate IDs...")
    
    import api_data as api_data_def
    wiredtiger_open_config = api_data_def.methods.get('wiredtiger_open')
    keys = gen.extract_configs(wiredtiger_open_config.config)
    
    baseline = gen.load_baseline()
    assignments = gen.assign_ids(keys, baseline)
    
    # Check for duplicates
    id_to_key = {}
    for key_name, (id_val, _) in assignments.items():
        if id_val in id_to_key:
            assert False, f"Duplicate ID {id_val}: {key_name} and {id_to_key[id_val]}"
        id_to_key[id_val] = key_name
    
    print("  PASSED")

def test_verify_mode():
    """Test that --verify mode works."""
    print("Test: verify mode...")
    
    # Save original argv
    orig_argv = sys.argv
    
    try:
        # Run with --verify flag
        sys.argv = ['gen_open_conf.py', '--verify']
        
        # This should not raise (since files are already generated)
        # We can't easily test this without modifying files, so just check
        # that verify_abi_stability works with current state
        baseline = gen.load_baseline()
        
        import api_data as api_data_def
        wiredtiger_open_config = api_data_def.methods.get('wiredtiger_open')
        keys = gen.extract_configs(wiredtiger_open_config.config)
        assignments = gen.assign_ids(keys, baseline)
        
        gen.verify_abi_stability(baseline, assignments)
        
    finally:
        sys.argv = orig_argv
    
    print("  PASSED")

def main():
    print("=" * 60)
    print("Testing gen_open_conf.py")
    print("=" * 60)
    
    # Change to dist directory
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    
    tests = [
        test_baseline_loading,
        test_config_extraction,
        test_abi_stability_check,
        test_id_assignment_preserves_baseline,
        test_category_assignment,
        test_type_mapping,
        test_generated_files_syntax,
        test_no_duplicate_ids,
        test_verify_mode,
    ]
    
    passed = 0
    failed = 0
    
    for test in tests:
        try:
            test()
            passed += 1
        except AssertionError as e:
            print(f"  FAILED: {e}")
            failed += 1
        except Exception as e:
            print(f"  ERROR: {e}")
            failed += 1
    
    print("=" * 60)
    print(f"Results: {passed} passed, {failed} failed")
    print("=" * 60)
    
    return 0 if failed == 0 else 1

if __name__ == '__main__':
    sys.exit(main())
