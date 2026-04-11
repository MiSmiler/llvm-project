# haha-potential-null-ptr-deref Check

## Introduction

`haha-potential-null-ptr-deref` is a clang-tidy check that detects potential null pointer dereferences. It uses Clang's data flow analysis framework to track the nullness state of pointers and emits warnings when a pointer that may be null is dereferenced.

## Features

- Detects `->` and `*` dereference operations on raw pointers
- Detects `operator->` and `operator*` on smart pointers (`std::unique_ptr`, `std::shared_ptr`)
- Uses data flow analysis to track pointer nullness state
- Supports control flow sensitive nullness propagation (e.g., `if (p)` branches)
- Strict mode: assumes unknown function return values may be nullptr

## Building

### Prerequisites

1. Compiled LLVM/Clang project
2. CMake and Ninja (or other build tools)

### Build Steps

```bash
# Enter LLVM project directory
cd /home/hui/coding/myproj/llvm-project

# If using Ninja build
ninja clangTidyHahaModule

# Or use generic CMake build
cmake --build . --target clangTidyHahaModule
```

### Full clang-tidy Build

If you need to build the complete clang-tidy tool:

```bash
ninja clang-tidy
```

## Testing

### Running Unit Tests

NOTE: Run these commands from the build directory (`cd build` first)

```bash
# Run haha module tests using llvm-lit
bin/llvm-lit -v ../clang-tools-extra/test/clang-tidy/checkers/haha/

# Or run all clang-tools tests (includes haha)
ninja check-clang-tools
```

### Manual Testing

Create a test file `test.cpp`:

```cpp
struct Info {
    int value;
};

void may_init(Info** p);
Info* get_info();

void test1() {
    Info* p = nullptr;
    may_init(&p);
    // Warning: p may still be nullptr
    int v = p->value;
}

void test2() {
    Info* p = nullptr;
    may_init(&p);
    if (p) {
        // Safe: p has been checked not null
        int v = p->value;
    }
}
```

Run the check:

```bash
clang-tidy --checks='haha-potential-null-ptr-deref' test.cpp
```

## Usage

### Command Line Usage

```bash
# Run check on a single file
clang-tidy --checks='haha-potential-null-ptr-deref' source.cpp

# Run on multiple files
clang-tidy --checks='haha-potential-null-ptr-deref' file1.cpp file2.cpp

# Use with compilation database
clang-tidy --checks='haha-potential-null-ptr-deref' -p compile_commands.json source.cpp
```

### Using .clang-tidy Configuration File

Create a `.clang-tidy` file in the project root directory:

```yaml
Checks: 'haha-potential-null-ptr-deref'
WarningsAsErrors: 'haha-potential-null-ptr-deref'
```

### Combining with Other Checks

```bash
# Run multiple checks simultaneously
clang-tidy --checks='haha-potential-null-ptr-deref,bugprone-*' source.cpp

# Exclude certain checks
clang-tidy --checks='haha-potential-null-ptr-deref,-bugprone-unused-return-value' source.cpp
```

## Example Output

### Code That Produces Warnings

```cpp
void example1() {
    int* p = nullptr;
    // ...
    int x = *p;  // warning: potential null pointer dereference
}
```

Output:
```
test.cpp:4:12: warning: potential null pointer dereference; pointer may be null at this point [haha-potential-null-ptr-deref]
```

### Code That Does Not Produce Warnings

```cpp
void example2() {
    int* p = nullptr;
    // ...
    if (p != nullptr) {
        int x = *p;  // OK: p has been checked not null
    }
}

void example3() {
    int* p = new int();  // OK: new always returns non-null
    int x = *p;
}
```

## Detected Dereference Forms

| Form | Description |
|------|------|
| `p->member` | Raw pointer arrow operator |
| `*p` | Raw pointer dereference |
| `smart_ptr->member` | Smart pointer arrow operator |
| `*smart_ptr` | Smart pointer dereference |

**Note**: Array subscript operation `ptr[i]` is not detected because it is too common and generally considered safe.

## Strict Mode Explanation

The check uses strict mode to handle pointers from unknown sources:

- **Assumed possibly null**:
  - Function parameters (unless marked with `_Nonnull` attribute)
  - Function return values (unless from known non-null functions like `new`, `std::make_unique`, etc.)

- **Assumed non-null**:
  - Pointers returned by `new` expressions
  - Smart pointers returned by `std::make_unique`/`std::make_shared`
  - Pointers with `_Nonnull` or `__nonnull` attributes

## File Structure

```
clang-tools-extra/clang-tidy/haha/
├── CMakeLists.txt                 # Build configuration
├── HahaTidyModule.cpp            # Module registration
├── PotentialNullPtrDerefCheck.h  # Check header
├── PotentialNullPtrDerefCheck.cpp # Check implementation (with data flow analysis model)
└── README.md                      # This document
```

## Related Documentation

- clang-tidy user documentation: `clang-tools-extra/docs/clang-tidy/checks/haha/potential-null-ptr-deref.rst`
- Test file: `clang-tools-extra/test/clang-tidy/checkers/haha/potential-null-ptr-deref.cpp`