#pragma once

// Use only for small, measured hot paths. Keep debug builds easy to step through.
#if defined(NDEBUG) && defined(_MSC_VER)
#define NT_FORCE_INLINE __forceinline
#elif defined(NDEBUG) && (defined(__clang__) || defined(__GNUC__))
#define NT_FORCE_INLINE inline __attribute__((always_inline))
#else
#define NT_FORCE_INLINE inline
#endif
