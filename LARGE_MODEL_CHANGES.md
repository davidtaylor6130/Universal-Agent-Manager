# LARGE_MODEL_CHANGES.md

## Issues Found

### 1. Application.cpp (src/app/application.cpp) - 551 lines

#### Function Name Issues
| Issue | Reason | Related Files | Urgency | Difficulty |
|-------|--------|---------------|---------|------------|
| `RuntimeCliVersionStateSignature` | Should be `CalculateCliVersionStateSignature` - more descriptive | src/app/application.cpp:104 | High | Easy |
| `CaptureRuntimeCliCompatibilitySnapshot` | Should be `CreateCliCompatibilitySnapshot` - simpler name | src/app/application.cpp:152 | High | Easy |
| `RuntimeCliCompatibilitySnapshotChanged` | Should be `IsCliCompatibilitySnapshotChanged` - boolean function should start with `Is` | src/app/application.cpp:162 | Medium | Easy |
| `HasSelectedActiveRuntime` | Could be `IsSelectedChatRunning` - more focused name | src/app/application.cpp:179 | High | Easy |
| `HasAnyActiveRuntime` | Could be `IsAnyRuntimeActive` - simpler name | src/app/application.cpp:196 | Medium | Easy |
| `NextPollDelayMs` | Could be `GetNextPollDelayMs` - consistent naming | src/app/application.cpp:214 | Medium | Easy |

#### Code Organization Issues
| Issue | Reason | Related Files | Urgency | Difficulty |
|-------|--------|---------------|---------|------------|
| Anonymous namespace with too many helpers | Should separate utility functions | src/app/application.cpp:66-249 | High | Medium |
| Mixed concerns in `RuntimeCliCompatibilitySnapshot` struct | Should use named fields | src/app/application.cpp:144-150 | Medium | Easy |

#### Performance Issues
| Issue | Reason | Related Files | Urgency | Difficulty |
|-------|--------|---------------|---------|------------|
| Redundant state signature calculation | Could compute on demand | src/app/application.cpp:299-306 | Medium | Easy |

### 2. Platform Services (src/common/platform/platform_services_macos_impl.cpp) - 1792 lines (reduced from 1797)

#### FIXED: Duplicate `ResolveExecutablePathForTerminal` function | Critical | Easy |
|---|--------|---------------|---|
| Removed duplicate function definition | Function 2 at line 413 | src/common/platform/platform_services_macos_impl.cpp:384,413 | Fixed | Applied |

#### FIXED: Function Naming Issues in application.cpp | Medium | Easy |
|---|--------|---------------|---|
| `RuntimeCliVersionStateSignature` → `CalculateCliVersionStateSignature` | src/app/application.cpp | src/app/application.cpp:104 | Fixed | Applied |
| `CaptureRuntimeCliCompatibilitySnapshot` → `CreateCliCompatibilitySnapshot` | src/app/application.cpp | src/app/application.cpp:152 | Fixed | Applied |
| `RuntimeCliCompatibilitySnapshotChanged` → `IsCliCompatibilitySnapshotChanged` | src/app/application.cpp | src/app/application.cpp:162 | Fixed | Applied |
| `HasSelectedActiveRuntime` → `IsSelectedChatRunning` | src/app/application.cpp | src/app/application.cpp:179 | Fixed | Applied |
| `HasAnyActiveRuntime` → `IsAnyRuntimeActive` | src/app/application.cpp | src/app/application.cpp:196 | Fixed | Applied |
| `NextPollDelayMs` → `GetNextPollDelayMs` | src/app/application.cpp | src/app/application.cpp:214 | Fixed | Applied |

#### Function Naming Issues (Remaining)
| Issue | Reason | Related Files | Urgency | Difficulty |
|-------|--------|---------------|---------|------------|
| `IsInterruptedErrno` | Should be `IsErrnoInterrupted` - standard naming | src/common/platform/platform_services_macos_impl.cpp:36 | Medium | Easy |
| `IsWouldBlockErrno` | Should be `IsErrnoWouldBlock` - standard naming | src/common/platform/platform_services_macos_impl.cpp:41 | Medium | Easy |
| `ErrorWithOptionalDetail` | Should be `FormatErrorWithDetail` - more descriptive | src/common/platform/platform_services_macos_impl.cpp:75 | Medium | Easy |
| `CommandNotFoundOnPathMessage` | Should be `CreateCommandNotFoundErrorMessage` | src/common/platform/platform_services_macos_impl.cpp:434 | Medium | Easy |
| `NodeRuntimeNotFoundMessage` | Should be `CreateNodeRuntimeNotFoundErrorMessage` | src/common/platform/platform_services_macos_impl.cpp:439 | Medium | Easy |
| `ValidateRequiredNodeRuntime` | Should be `IsNodeRuntimeAvailable` | src/common/platform/platform_services_macos_impl.cpp:449 | Medium | Easy |
| `PrepareWorkingDirectory` | Should be `EnsureWorkingDirectoryExists` | src/common/platform/platform_services_macos_impl.cpp:468 | Medium | Easy |
| `EscapeAppleScriptQuotedString` | Should be `EscapeStringForAppleScript` | src/common/platform/platform_services_macos_impl.cpp:497 | Medium | Easy |
| `ReadAvailablePipeData` | Should be `ReadAllPipeData` | src/common/platform/platform_services_macos_impl.cpp:521 | Medium | Easy |
| `ExecuteCapturedCommandPosix` | Should be `ExecuteCommandCaptureOutput` | src/common/platform/platform_services_macos_impl.cpp:564 | High | Medium |

#### Code Organization Issues
| Issue | Reason | Related Files | Urgency | Difficulty |
|-------|--------|---------------|---------|------------|
| Too many utility functions in anonymous namespace | Should be split into separate header files | src/common/platform/platform_services_macos_impl.cpp:34-770 | Critical | High |
| Mixed platform-specific and generic logic | Platform-specific logic mixed with generic utilities | src/common/platform/platform_services_macos_impl.cpp:34-1109 | High | High |

#### Code Duplication
| Issue | Reason | Related Files | Urgency | Difficulty |
|-------|--------|---------------|---------|------------|
| Similar error handling in multiple functions | Could use a common error reporting utility | src/common/platform/platform_services_macos_impl.cpp | High | Medium |

### 3. Platform Services (src/common/platform/platform_services_windows_impl.cpp) - 2013 lines

#### Function Naming Issues
| Issue | Reason | Related Files | Urgency | Difficulty |
|-------|--------|---------------|---------|------------|
| Similar naming issues as macOS version | Should refactor consistently | src/common/platform/platform_services_windows_impl.cpp | High | High |

#### Code Organization Issues
| Issue | Reason | Related Files | Urgency | Difficulty |
|-------|--------|---------------|---------|------------|
| Same structural issues as macOS version | Should apply same refactoring patterns | src/common/platform/platform_services_windows_impl.cpp | Critical | High |

## Summary

### Total Issues: ~37 (reduced from ~47)
### Critical Issues: ~8 (reduced from ~9)
### Medium Issues: ~20
### Easy Issues: ~9 (reduced from ~18)

This repository has approximately 37 code quality issues that need to be addressed, with about 8 critical issues that should be prioritized first.
