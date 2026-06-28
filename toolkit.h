#if !defined(MG_TOOLKIT)

#include "types.h"

#include <format>
#include <iostream>

#define array_len(arr) (sizeof(arr) / sizeof((arr)[0]))

//
//  Pseudo-random generator
//
struct Rng
{
  u64 state;

  u64
  get_u64()
  {
    state = state * 0x9e3779b97f4a7c15 + 0x85ebca6b7c4f4b23;
    return state;
  }
  f32
  get_f32()
  {
    return (f32)(this->get_u64() >> 40) / (f32)(1 << 24);
  }
};

//
// Logging
//
template <typename... Args>
void
log_raw(std::format_string<Args...> fmt, Args &&...args)
{
  std::string out = std::format(fmt, std::forward<Args>(args)...) + '\n';
  std::cout << out;
}

template <typename... Args>
void
log_info(std::format_string<Args...> fmt, Args &&...args)
{
  std::string out = "[i] " + std::format(fmt, std::forward<Args>(args)...) + '\n';
  std::cout << out;
}

template <typename A, typename B, typename... C>
void
log_info(std::format_string<A, B, C...> &fmt, A a, B b, C... c)
{
  std::string out = "[i] " + std::format_to(fmt, std::forward<A, B, C...>(a, b, c)...) + '\n';
  std::cout << out;
}

#define MG_TOOLKIT
#endif
