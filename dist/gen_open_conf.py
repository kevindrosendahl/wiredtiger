#!/usr/bin/env python3
#
# gen_open_conf.py --
#     Generate wiredtiger_open_conf.h and conn_open_conf.c from api_data.py.
#
# This script reads the wiredtiger_open configuration definitions from api_data.py
# and generates:
#   - src/include/wiredtiger_open_conf.h: Public key ID definitions
#   - src/conn/conn_open_conf.c: Key mapping table
#
# ABI Stability:
#   Existing key IDs are NEVER changed. This is enforced by loading a baseline
#   file and verifying no existing IDs are modified. New keys are assigned IDs
#   from their category's range.

import os
import sys
import re
from collections import OrderedDict

# Import dist utilities
from dist import compare_srcfile

# Import api_data for configuration definitions
import api_data as api_data_def

# =============================================================================
# Configuration Categories and ID Ranges
# =============================================================================
#
# Each category has a reserved range of 100 IDs. This allows for growth without
# renumbering existing keys.

CATEGORY_RANGES = OrderedDict([
    ('core',         (1000, 1099)),  # Top-level core options
    ('eviction',     (1100, 1199)),  # eviction.* options
    ('log',          (1200, 1299)),  # log.* options
    ('checkpoint',   (1300, 1399)),  # checkpoint.* and checkpoint_cleanup.* options
    ('statistics',   (1400, 1499)),  # statistics and statistics_log.* options
    ('file_manager', (1500, 1599)),  # file_manager.* options
    ('debug_mode',   (1600, 1699)),  # debug_mode.* options
    ('compatibility',(1700, 1799)),  # compatibility.* options
    ('prefetch',     (1800, 1899)),  # prefetch.* options
    ('live_restore', (1900, 1999)),  # live_restore.* options
    ('encryption',   (2000, 2099)),  # encryption.* options
    ('chunk_cache',  (2100, 2199)),  # chunk_cache.* options
    ('tiered',       (2200, 2299)),  # tiered_storage.* options
    ('hash',         (2300, 2399)),  # hash.* options
    ('transaction_sync', (2400, 2499)),  # transaction_sync.* options
    ('io_capacity',  (2500, 2599)),  # io_capacity.* options
    ('history_store',(2600, 2699)),  # history_store.* options
    ('shared_cache', (2700, 2799)),  # shared_cache.* options
    ('block_cache',  (2800, 2899)),  # block_cache.* options
])

# Map parent config names to categories
PARENT_TO_CATEGORY = {
    None: 'core',
    'eviction': 'eviction',
    'log': 'log',
    'checkpoint': 'checkpoint',
    'checkpoint_cleanup': 'checkpoint',
    'statistics_log': 'statistics',
    'file_manager': 'file_manager',
    'debug_mode': 'debug_mode',
    'compatibility': 'compatibility',
    'prefetch': 'prefetch',
    'live_restore': 'live_restore',
    'encryption': 'encryption',
    'chunk_cache': 'chunk_cache',
    'tiered_storage': 'tiered',
    'hash': 'hash',
    'transaction_sync': 'transaction_sync',
    'io_capacity': 'io_capacity',
    'history_store': 'history_store',
    'shared_cache': 'shared_cache',
    'block_cache': 'block_cache',
}

# Config names that should be treated as top-level (no parent) even though
# they look like they might be nested. These are configs that appear at the
# top level of wiredtiger_open config, not under a category.
TOP_LEVEL_CONFIGS = {
    'eviction_target', 'eviction_trigger', 'eviction_dirty_target',
    'eviction_dirty_trigger', 'eviction_updates_target', 'eviction_updates_trigger',
    'eviction_checkpoint_target', 'statistics',
}

# Map config names to their category for ID assignment.
# Top-level eviction_* configs go in 'core', nested eviction.* go in 'eviction'
CONFIG_CATEGORY_OVERRIDES = {
    'eviction_target': 'core',
    'eviction_trigger': 'core',
    'eviction_dirty_target': 'core',
    'eviction_dirty_trigger': 'core',
    'eviction_updates_target': 'core',
    'eviction_updates_trigger': 'core',
    'eviction_checkpoint_target': 'core',
    'statistics': 'core',
}

# =============================================================================
# Type Mapping
# =============================================================================

def get_wt_type(config):
    """Map api_data config type to WT_OPEN_CONFIG_ARG_* type."""
    flags = config.flags
    ctype = flags.get('type', None)
    if ctype == 'boolean':
        return 'WT_OPEN_CONFIG_ARG_BOOL'
    elif ctype == 'int':
        return 'WT_OPEN_CONFIG_ARG_INT'
    elif ctype == 'category':
        return None  # Categories are not directly configurable
    elif ctype == 'list':
        return 'WT_OPEN_CONFIG_ARG_STR'  # Lists are passed as strings
    else:
        # Check if it has min/max (implies int)
        if 'min' in flags or 'max' in flags:
            return 'WT_OPEN_CONFIG_ARG_INT'
        return 'WT_OPEN_CONFIG_ARG_STR'  # Default to string

def get_type_comment(config):
    """Get a brief type comment for the header file."""
    flags = config.flags
    ctype = flags.get('type', None)
    if ctype == 'boolean':
        return 'bool'
    elif ctype == 'int':
        return 'int'
    elif ctype == 'list':
        return 'string (list)'
    elif ctype == 'category':
        return 'category'
    elif 'min' in flags or 'max' in flags:
        return 'int'
    return 'string'

# =============================================================================
# Config Extraction
# =============================================================================

class ConfigKey:
    """Represents a single configuration key for wiredtiger_open_ex."""
    def __init__(self, name, key_name, parent_name, wt_type, type_comment, description):
        self.name = name              # Full key name (e.g., "log_enabled")
        self.key_name = key_name      # String name for lookup (e.g., "enabled")
        self.parent_name = parent_name  # Parent category (e.g., "log") or None
        self.wt_type = wt_type        # WT_OPEN_CONFIG_ARG_* type
        self.type_comment = type_comment  # Type for comments
        self.description = description  # Brief description
        self.id = None                # Assigned ID (set later)
        self.category = None          # Category name (set later)

def extract_configs(configs, parent_name=None, parent_key_prefix=''):
    """
    Extract all configuration keys from api_data configs.
    
    Recursively processes nested categories.
    Returns a list of ConfigKey objects.
    """
    keys = []
    
    for config in configs:
        config_type = config.flags.get('type', None)
        
        if config_type == 'category':
            # This is a nested category - recurse into subconfigs
            if config.subconfig:
                subkeys = extract_configs(
                    config.subconfig,
                    parent_name=config.name,
                    parent_key_prefix=config.name + '_'
                )
                keys.extend(subkeys)
        else:
            # This is a leaf config - create a key for it
            wt_type = get_wt_type(config)
            if wt_type is None:
                continue  # Skip unsupported types
            
            # Determine if this is a top-level key or nested
            if config.name in TOP_LEVEL_CONFIGS:
                # These are top-level even though they have a prefix
                full_name = config.name
                key_name = config.name
                actual_parent = None
            elif parent_name is not None:
                # Nested config
                full_name = parent_key_prefix + config.name
                key_name = config.name
                actual_parent = parent_name
            else:
                # Top-level config
                full_name = config.name
                key_name = config.name
                actual_parent = None
            
            # Get description (first line only, cleaned up)
            desc = config.desc.strip().split('\n')[0].strip()
            if len(desc) > 50:
                desc = desc[:47] + '...'
            
            key = ConfigKey(
                name=full_name,
                key_name=key_name,
                parent_name=actual_parent,
                wt_type=wt_type,
                type_comment=get_type_comment(config),
                description=desc
            )
            keys.append(key)
    
    return keys

def determine_category(key):
    """
    Determine the category for a key based on its name and parent.
    """
    # Check explicit overrides first
    if key.name in CONFIG_CATEGORY_OVERRIDES:
        return CONFIG_CATEGORY_OVERRIDES[key.name]
    
    # Otherwise, use parent name to determine category
    parent = key.parent_name
    return PARENT_TO_CATEGORY.get(parent, 'core')

# =============================================================================
# Baseline Management (ABI Stability)
# =============================================================================

# Use path relative to this script's location so it works regardless of cwd
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BASELINE_FILE = os.path.join(_SCRIPT_DIR, '..', 'src', 'include', 'wiredtiger_open_conf.h.baseline')

def load_baseline():
    """
    Load the baseline key ID assignments.
    Returns a dict of {key_name: id}.
    """
    baseline = {}
    if not os.path.exists(BASELINE_FILE):
        return baseline
    
    with open(BASELINE_FILE, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            if '=' in line:
                key, id_str = line.split('=', 1)
                baseline[key.strip()] = int(id_str.strip())
    
    return baseline

def save_baseline(key_assignments):
    """
    Save the key ID assignments to the baseline file.
    """
    with open(BASELINE_FILE, 'w') as f:
        f.write("# WiredTiger Open Configuration Key ID Baseline\n")
        f.write("# DO NOT EDIT - This file is auto-generated by dist/gen_open_conf.py\n")
        f.write("#\n")
        f.write("# This file defines the stable key ID assignments for wiredtiger_open_ex.\n")
        f.write("# Key IDs are NEVER changed once assigned - this ensures ABI stability.\n")
        f.write("# New keys are added at the end of their category's range.\n")
        f.write("#\n")
        
        # Group by category for readability
        by_category = {}
        for key_name, (id_val, category) in key_assignments.items():
            if category not in by_category:
                by_category[category] = []
            by_category[category].append((key_name, id_val))
        
        for category in CATEGORY_RANGES.keys():
            if category not in by_category:
                continue
            range_start, range_end = CATEGORY_RANGES[category]
            f.write(f"\n# {category.upper()} ({range_start}-{range_end})\n")
            for key_name, id_val in sorted(by_category[category], key=lambda x: x[1]):
                f.write(f"{key_name}={id_val}\n")

def verify_abi_stability(baseline, new_assignments):
    """
    Verify that no existing key IDs have changed.
    Raises an error if ABI stability is violated.
    """
    errors = []
    for key_name, old_id in baseline.items():
        if key_name in new_assignments:
            new_id = new_assignments[key_name][0]
            if new_id != old_id:
                errors.append(
                    f"ABI BREAK: {key_name} ID changed from {old_id} to {new_id}"
                )
    
    if errors:
        print("ERROR: ABI stability violation detected!", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        print("\nKey IDs must NEVER change. If you need to change a key,", file=sys.stderr)
        print("deprecate the old one and add a new key instead.", file=sys.stderr)
        sys.exit(1)

# =============================================================================
# ID Assignment
# =============================================================================

def assign_ids(keys, baseline):
    """
    Assign IDs to all keys, preserving baseline assignments.
    Returns a dict of {key_name: (id, category)}.
    """
    assignments = {}
    
    # Track next available ID for each category
    next_id = {}
    for category, (range_start, range_end) in CATEGORY_RANGES.items():
        next_id[category] = range_start
    
    # First pass: Apply baseline assignments and update next_id trackers
    for key in keys:
        full_key_name = f"WT_OPEN_CONF_{key.name}"
        if full_key_name in baseline:
            key.id = baseline[full_key_name]
            # Determine category from the ID range
            for cat, (range_start, range_end) in CATEGORY_RANGES.items():
                if range_start <= key.id <= range_end:
                    key.category = cat
                    # Update next_id to avoid conflicts
                    if key.id >= next_id[cat]:
                        next_id[cat] = key.id + 1
                    break
            assignments[full_key_name] = (key.id, key.category)
    
    # Also scan baseline for IDs not in current keys (to preserve gaps)
    for full_key_name, id_val in baseline.items():
        for cat, (range_start, range_end) in CATEGORY_RANGES.items():
            if range_start <= id_val <= range_end:
                if id_val >= next_id[cat]:
                    next_id[cat] = id_val + 1
                break
    
    # Second pass: Assign IDs to new keys not in baseline
    for key in keys:
        if key.id is not None:
            continue  # Already assigned from baseline
        
        # Determine category using our category logic
        category = determine_category(key)
        key.category = category
        
        # Assign next available ID in the category
        range_start, range_end = CATEGORY_RANGES[category]
        if next_id[category] > range_end:
            print(f"ERROR: Category '{category}' has run out of IDs!", file=sys.stderr)
            print(f"  Range: {range_start}-{range_end}", file=sys.stderr)
            print(f"  Key: {key.name}", file=sys.stderr)
            sys.exit(1)
        
        key.id = next_id[category]
        next_id[category] += 1
        
        full_key_name = f"WT_OPEN_CONF_{key.name}"
        assignments[full_key_name] = (key.id, key.category)
    
    return assignments

# =============================================================================
# Code Generation
# =============================================================================

def generate_header(keys, output_file):
    """Generate wiredtiger_open_conf.h."""
    
    # Group keys by category
    by_category = {}
    for key in keys:
        if key.category not in by_category:
            by_category[key.category] = []
        by_category[key.category].append(key)
    
    lines = []
    lines.append("/*-")
    lines.append(" * Copyright (c) 2014-present MongoDB, Inc.")
    lines.append(" * Copyright (c) 2008-2014 WiredTiger, Inc.")
    lines.append(" *\tAll rights reserved.")
    lines.append(" *")
    lines.append(" * See the file LICENSE for redistribution information.")
    lines.append(" */")
    lines.append("")
    lines.append("/*")
    lines.append(" * DO NOT EDIT: automatically built by dist/gen_open_conf.py.")
    lines.append(" */")
    lines.append("")
    lines.append("#ifndef __WIREDTIGER_OPEN_CONF_H_")
    lines.append("#define __WIREDTIGER_OPEN_CONF_H_")
    lines.append("")
    lines.append("/*")
    lines.append(" * Public configuration key IDs for use with wiredtiger_open_ex.")
    lines.append(" *")
    lines.append(" * NAMING: These use WT_OPEN_CONF_* prefix (not WT_CONF_ID_*) to clearly")
    lines.append(" * distinguish from the internal WT_CONF_ID_* identifiers in conf_keys.h.")
    lines.append(" * The internal IDs use bit-packing for nested keys; these are flat and stable.")
    lines.append(" *")
    lines.append(" * ABI STABILITY POLICY:")
    lines.append(" * - Key ID values are explicit and will NEVER change between versions")
    lines.append(" * - New options get new IDs; existing IDs are NEVER removed or reused")
    lines.append(" * - Deprecated keys: ID is retained forever, usage produces a warning,")
    lines.append(" *   behavior may become a no-op")
    lines.append(" * - Unknown keys: wiredtiger_open_ex returns EINVAL with descriptive message")
    lines.append(" *")
    lines.append(" * Key ranges:")
    for category, (range_start, range_end) in CATEGORY_RANGES.items():
        lines.append(f" *   {range_start}-{range_end}: {category.replace('_', ' ').title()} options")
    lines.append(" */")
    lines.append("")
    lines.append("/*")
    lines.append(" * Sentinel value for array terminator.")
    lines.append(" * Using UINT64_MAX is safer than 0 since 0 could theoretically be a valid key.")
    lines.append(" */")
    lines.append("#ifndef WT_OPEN_CONF_KEY_END")
    lines.append("#define WT_OPEN_CONF_KEY_END UINT64_MAX")
    lines.append("#endif")
    
    # Generate defines for each category
    for category in CATEGORY_RANGES.keys():
        if category not in by_category:
            continue
        
        range_start, range_end = CATEGORY_RANGES[category]
        lines.append("")
        lines.append(f"/* {'=' * 76}")
        lines.append(f" * {category.replace('_', ' ').title()} options ({range_start}-{range_end})")
        lines.append(f" * {'=' * 76} */")
        
        for key in sorted(by_category[category], key=lambda k: k.id):
            # Build the define line
            define_name = f"WT_OPEN_CONF_{key.name}"
            # Align the ID value
            padding = max(1, 48 - len(define_name))
            comment = f"/* {key.type_comment}: {key.description} */"
            lines.append(f"#define {define_name}{' ' * padding}{key.id}  {comment}")
    
    lines.append("")
    lines.append("#endif /* __WIREDTIGER_OPEN_CONF_H_ */")
    lines.append("")
    
    content = '\n'.join(lines)
    
    # Write to temp file and compare
    tmp_file = f'__tmp_gen_open_conf_{os.getpid()}'
    with open(tmp_file, 'w') as f:
        f.write(content)
    
    compare_srcfile(tmp_file, output_file)

def generate_mapping(keys, output_file):
    """Generate conn_open_conf.c."""
    
    # Group keys by category
    by_category = {}
    for key in keys:
        if key.category not in by_category:
            by_category[key.category] = []
        by_category[key.category].append(key)
    
    lines = []
    lines.append("/*-")
    lines.append(" * Copyright (c) 2014-present MongoDB, Inc.")
    lines.append(" * Copyright (c) 2008-2014 WiredTiger, Inc.")
    lines.append(" *\tAll rights reserved.")
    lines.append(" *")
    lines.append(" * See the file LICENSE for redistribution information.")
    lines.append(" */")
    lines.append("")
    lines.append("/*")
    lines.append(" * DO NOT EDIT: automatically built by dist/gen_open_conf.py.")
    lines.append(" */")
    lines.append("")
    lines.append('#include "wt_internal.h"')
    lines.append('#include "wiredtiger_open_conf.h"')
    lines.append("")
    lines.append("/*")
    lines.append(" * Maps public WT_OPEN_CONF_* keys to internal configuration paths.")
    lines.append(" *")
    lines.append(" * For nested keys (e.g., eviction.threads_min), parent_name specifies")
    lines.append(" * the parent category. The lookup code will:")
    lines.append(" * 1. Get the parent config item")
    lines.append(" * 2. Use __wt_config_subgets to get the nested value")
    lines.append(" *")
    lines.append(" * Note: Default values are NOT stored here. They are managed by the")
    lines.append(" * existing configuration system (dist/api_config.py generates them")
    lines.append(" * into the base config strings). For struct config, \"key not present\"")
    lines.append(" * means \"use default from base config\".")
    lines.append(" */")
    lines.append("typedef struct {")
    lines.append("    uint64_t public_key;      /* WT_OPEN_CONF_* */")
    lines.append("    const char *key_name;     /* String name (e.g., \"cache_size\", \"threads_min\") */")
    lines.append("    const char *parent_name;  /* Parent category (NULL if top-level) */")
    lines.append("    uint8_t expected_type;    /* Expected WT_OPEN_CONFIG_ARG type */")
    lines.append("} WT_OPEN_CONF_KEY_MAP;")
    lines.append("")
    lines.append("/*")
    lines.append(" * Key mappings for wiredtiger_open_ex.")
    lines.append(" * All supported configuration keys.")
    lines.append(" */")
    lines.append("static const WT_OPEN_CONF_KEY_MAP open_conf_key_map[] = {")
    
    # Generate entries for each category
    for category in CATEGORY_RANGES.keys():
        if category not in by_category:
            continue
        
        range_start, range_end = CATEGORY_RANGES[category]
        lines.append(f"    /* {'=' * 74}")
        lines.append(f"     * {category.replace('_', ' ').title()} options ({range_start}-{range_end})")
        lines.append(f"     * {'=' * 74} */")
        
        for key in sorted(by_category[category], key=lambda k: k.id):
            define_name = f"WT_OPEN_CONF_{key.name}"
            parent_str = f'"{key.parent_name}"' if key.parent_name else "NULL"
            lines.append(f'    {{{define_name}, "{key.key_name}", {parent_str}, {key.wt_type}}},')
    
    lines.append("")
    lines.append("    /* Terminator */")
    lines.append("    {WT_OPEN_CONF_KEY_END, NULL, NULL, 0}")
    lines.append("};")
    lines.append("")
    
    # Add the lookup and utility functions
    lines.append("/*")
    lines.append(" * __wt_open_conf_key_lookup --")
    lines.append(" *     Look up a public key ID in the mapping table.")
    lines.append(" *     Returns NULL if not found.")
    lines.append(" *")
    lines.append(" * Performance: Linear scan. For ~100 keys this is negligible.")
    lines.append(" * The table is sorted by public_key so binary search could be")
    lines.append(" * added if needed, but startup config lookup is not a hot path.")
    lines.append(" */")
    lines.append("static const WT_OPEN_CONF_KEY_MAP *")
    lines.append("__wt_open_conf_key_lookup(uint64_t public_key)")
    lines.append("{")
    lines.append("    const WT_OPEN_CONF_KEY_MAP *entry;")
    lines.append("")
    lines.append("    for (entry = open_conf_key_map; entry->key_name != NULL; entry++) {")
    lines.append("        if (entry->public_key == public_key)")
    lines.append("            return (entry);")
    lines.append("    }")
    lines.append("    return (NULL);")
    lines.append("}")
    lines.append("")
    lines.append("/*")
    lines.append(" * __wt_open_conf_type_name --")
    lines.append(" *     Return a human-readable name for a config arg type.")
    lines.append(" */")
    lines.append("static const char *")
    lines.append("__wt_open_conf_type_name(uint8_t type)")
    lines.append("{")
    lines.append("    switch (type) {")
    lines.append("    case WT_OPEN_CONFIG_ARG_INT:")
    lines.append('        return ("integer");')
    lines.append("    case WT_OPEN_CONFIG_ARG_BOOL:")
    lines.append('        return ("boolean");')
    lines.append("    case WT_OPEN_CONFIG_ARG_STR:")
    lines.append('        return ("string");')
    lines.append("    default:")
    lines.append('        return ("unknown");')
    lines.append("    }")
    lines.append("}")
    lines.append("")
    lines.append("/*")
    lines.append(" * __wt_open_conf_validate_args --")
    lines.append(" *     Validate all config args have known keys and correct types.")
    lines.append(" */")
    lines.append("int")
    lines.append("__wt_open_conf_validate_args(")
    lines.append("  WT_SESSION_IMPL *session, const WT_OPEN_CONFIG_ARG *args, size_t count)")
    lines.append("{")
    lines.append("    const WT_OPEN_CONFIG_ARG *arg;")
    lines.append("    const WT_OPEN_CONF_KEY_MAP *key_map;")
    lines.append("    size_t i;")
    lines.append("")
    lines.append("    if (args == NULL)")
    lines.append("        return (0);")
    lines.append("")
    lines.append("    if (count == 0) {")
    lines.append("        /* Sentinel-terminated array */")
    lines.append("        for (arg = args; arg->key != WT_OPEN_CONF_KEY_END; arg++) {")
    lines.append("            key_map = __wt_open_conf_key_lookup(arg->key);")
    lines.append("            if (key_map == NULL)")
    lines.append('                WT_RET_MSG(session, EINVAL, "Unknown configuration key ID: %" PRIu64, arg->key);')
    lines.append("")
    lines.append("            /* Type validation - allow bool where int expected */")
    lines.append("            if (arg->type != key_map->expected_type) {")
    lines.append("                if (!(key_map->expected_type == WT_OPEN_CONFIG_ARG_INT &&")
    lines.append("                      arg->type == WT_OPEN_CONFIG_ARG_BOOL)) {")
    lines.append("                    WT_RET_MSG(session, EINVAL,")
    lines.append('                      "Configuration key %s (ID %" PRIu64 "): expected type %s, got %s",')
    lines.append("                      key_map->key_name, arg->key, __wt_open_conf_type_name(key_map->expected_type),")
    lines.append("                      __wt_open_conf_type_name(arg->type));")
    lines.append("                }")
    lines.append("            }")
    lines.append("        }")
    lines.append("    } else {")
    lines.append("        /* Counted array */")
    lines.append("        for (i = 0; i < count; i++) {")
    lines.append("            arg = &args[i];")
    lines.append("            key_map = __wt_open_conf_key_lookup(arg->key);")
    lines.append("            if (key_map == NULL)")
    lines.append('                WT_RET_MSG(session, EINVAL, "Unknown configuration key ID: %" PRIu64, arg->key);')
    lines.append("")
    lines.append("            if (arg->type != key_map->expected_type) {")
    lines.append("                if (!(key_map->expected_type == WT_OPEN_CONFIG_ARG_INT &&")
    lines.append("                      arg->type == WT_OPEN_CONFIG_ARG_BOOL)) {")
    lines.append("                    WT_RET_MSG(session, EINVAL,")
    lines.append('                      "Configuration key %s (ID %" PRIu64 "): expected type %s, got %s",')
    lines.append("                      key_map->key_name, arg->key, __wt_open_conf_type_name(key_map->expected_type),")
    lines.append("                      __wt_open_conf_type_name(arg->type));")
    lines.append("                }")
    lines.append("            }")
    lines.append("        }")
    lines.append("    }")
    lines.append("")
    lines.append("    return (0);")
    lines.append("}")
    lines.append("")
    lines.append("/*")
    lines.append(" * __wt_open_conf_get_key_info --")
    lines.append(" *     Get the key name and parent name for a given key ID.")
    lines.append(" *     Returns WT_NOTFOUND if key is unknown.")
    lines.append(" */")
    lines.append("int")
    lines.append("__wt_open_conf_get_key_info(")
    lines.append("  uint64_t key_id, const char **key_namep, const char **parent_namep, uint8_t *expected_typep)")
    lines.append("{")
    lines.append("    const WT_OPEN_CONF_KEY_MAP *key_map;")
    lines.append("")
    lines.append("    key_map = __wt_open_conf_key_lookup(key_id);")
    lines.append("    if (key_map == NULL)")
    lines.append("        return (WT_NOTFOUND);")
    lines.append("")
    lines.append("    if (key_namep != NULL)")
    lines.append("        *key_namep = key_map->key_name;")
    lines.append("    if (parent_namep != NULL)")
    lines.append("        *parent_namep = key_map->parent_name;")
    lines.append("    if (expected_typep != NULL)")
    lines.append("        *expected_typep = key_map->expected_type;")
    lines.append("")
    lines.append("    return (0);")
    lines.append("}")
    lines.append("")
    
    content = '\n'.join(lines)
    
    # Write to temp file and compare
    tmp_file = f'__tmp_gen_open_conf_{os.getpid()}'
    with open(tmp_file, 'w') as f:
        f.write(content)
    
    compare_srcfile(tmp_file, output_file)

# =============================================================================
# Main
# =============================================================================

def main():
    # Parse command line
    verify_only = '--verify' in sys.argv
    update_baseline = '--update-baseline' in sys.argv
    
    # Extract wiredtiger_open configuration from api_data
    wiredtiger_open_config = api_data_def.methods.get('wiredtiger_open')
    if not wiredtiger_open_config or not wiredtiger_open_config.config:
        print("ERROR: Could not find wiredtiger_open configuration in api_data.py", file=sys.stderr)
        sys.exit(1)
    
    # Extract all keys
    keys = extract_configs(wiredtiger_open_config.config)
    
    print(f"Found {len(keys)} configuration keys for wiredtiger_open_ex")
    
    # Load baseline
    baseline = load_baseline()
    print(f"Loaded baseline with {len(baseline)} existing key assignments")
    
    # Assign IDs
    assignments = assign_ids(keys, baseline)
    
    # Verify ABI stability
    verify_abi_stability(baseline, assignments)
    print("ABI stability check passed")
    
    if verify_only:
        print("Verification complete (--verify mode)")
        return
    
    # Generate files - use paths relative to script location
    header_file = os.path.join(_SCRIPT_DIR, '..', 'src', 'include', 'wiredtiger_open_conf.h')
    mapping_file = os.path.join(_SCRIPT_DIR, '..', 'src', 'conn', 'conn_open_conf.c')
    
    print(f"Generating {header_file}...")
    generate_header(keys, header_file)
    
    print(f"Generating {mapping_file}...")
    generate_mapping(keys, mapping_file)
    
    # Update baseline if requested or if this is first run
    if update_baseline or not os.path.exists(BASELINE_FILE):
        print(f"Updating baseline file {BASELINE_FILE}...")
        save_baseline(assignments)
    
    print("Done!")

if __name__ == '__main__':
    main()
