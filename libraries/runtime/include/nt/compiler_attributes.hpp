#pragma once

// Use only for small, measured hot paths. Keep debug builds easy to step through.
#if defined(NDEBUG) && defined(_MSC_VER)
#define NT_FORCE_INLINE __forceinline
#elif defined(NDEBUG) && (defined(__clang__) || defined(__GNUC__))
#define NT_FORCE_INLINE inline __attribute__((always_inline))
#else
#define NT_FORCE_INLINE inline
#endif
// Shader numeric helpers return small structs. Inline them in SPIR-V so no
// struct crosses a function boundary as a wide integer the backend rejects.
#if defined(__SPIRV__)
#define NT_SHADER_INLINE inline __attribute__((always_inline))
#else
#define NT_SHADER_INLINE inline
#endif
