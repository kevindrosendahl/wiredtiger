# Agent Instructions

Project-specific guidance for AI coding agents working in this repository.

## Design Documents

Design documents and their reviews are stored in `designs/`.

**Structure:**
```
designs/
└── {feature-name}/
    ├── PLAN-{feature-name}.md              # The design document
    ├── PLAN-{feature-name}-review-*.md     # Design reviews
    └── PLAN-{feature-name}-impl-review-*.md # Implementation reviews
```

When creating a new design document:
1. Create a subdirectory under `designs/` named after the feature
2. Name the design document `PLAN-{feature-name}.md`
3. Reviews will be placed in the same directory automatically

## Code Style

This is the WiredTiger storage engine. Follow existing conventions:
- C code follows the project's `.clang-format`
- Use `WT_` prefix for public types and `__wt_` for internal functions
- Memory allocation uses `__wt_malloc`, `__wt_free`, etc.
- Error handling uses `WT_ERR`, `WT_RET`, `WT_TRET` macros

## Testing

- Unit tests go in `test/csuite/`
- Python tests go in `test/suite/`
- Run tests via `ninja` in the build directory
