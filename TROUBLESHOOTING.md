# Troubleshooting

### linking error due to LLVM dynamic linking errors

```
CommandLine Error: Option 'experimental-debuginfo-iterators' registered more than once!
LLVM ERROR: inconsistency in registered CommandLine options
[1]    26042 abort
```

When you encounter this error, it means you shall link against the LLVM dynamic library dynamic library.

Running Cmake with both `-DLINK_CLANG_DYLIB=ON` and `-DLINK_LLVM_DYLIB=ON` should fix the issue.
