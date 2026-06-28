#if !defined(MG_PROFILER)

#include "types.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>

//
// Add this after all PROFILER_SCOPE calls!
// This will redefine debug_records with size big enough to store all records
//
#define PROFILER_RECORDS_ARRAY DebugRecord debug_records[__COUNTER__]
#define PROFILER_SCOPE(name) ProfileScope _p_scope_##__LINE__ = ProfileScope(__COUNTER__, name, __FILE__, __LINE__)

//
// Platform-specific :: Win32
//
#if defined(_WIN32)
#if !defined(WINDOWS_LEAN_AND_MEAN)
#define WINDOWS_LEAN_AND_MEAN
#endif
#include <windows.h>
uint64_t
get_performance_counter()
{
  LARGE_INTEGER ticks;
  QueryPerformanceCounter(&ticks);
  return ticks.QuadPart;
}
uint64_t
get_performance_frequency()
{
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  return freq.QuadPart;
}

//
// Fallback for unsupported platforms
//
#else
uint64_t
get_performance_counter()
{
  return 1;
}
uint64_t
get_performance_frequency()
{
  return 1;
}
#endif

struct DebugRecord
{
  const char *name;
  const char *file;
  uint32_t line;

  std::atomic_uint64_t ticks;
  std::atomic_uint32_t hits;
};

static uint64_t g_profiler_performace_frequency;
// Forward-declaration, that must be redefined with PROFILER_RECORDS_ARRAY
DebugRecord debug_records[];

struct ProfileScope
{
  const char *name;
  const char *file;
  uint32_t line;
  uint32_t index;
  uint64_t start_ticks;

  ProfileScope(uint32_t p_index, const char *p_name, const char *p_file, uint32_t p_line)
  {
    index = p_index;
    name = p_name;
    file = p_file;
    line = p_line;
    start_ticks = get_performance_counter();
  }

  ~ProfileScope()
  {
    uint64_t elapsed_ticks = get_performance_counter() - start_ticks;
    DebugRecord *record = &debug_records[index];
    record->ticks.fetch_add(elapsed_ticks, std::memory_order_relaxed);
    record->hits.fetch_add(1, std::memory_order_relaxed);

    // All threads will write the same data, atomics are not needed here
    record->name = name;
    record->file = file;
    record->line = line;
  }
};

void
profiler_initialize()
{
  g_profiler_performace_frequency = get_performance_frequency();
}

float
milliseconds_from_ticks(uint64_t ticks)
{
  return (float)(1000.0 * (double)ticks / (double)(max(g_profiler_performace_frequency, 1)));
}

#define MG_PROFILER
#endif
