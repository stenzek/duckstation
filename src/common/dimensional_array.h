// Sourced from https://github.com/BreadFish64/ScaleFish/blob/master/common/dimensional_array.hpp
// Copyright (c) 2020 BreadFish64
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

namespace detail {
template<typename T, std::size_t rank, std::size_t... sizes>
struct DimensionalArrayExplicitRank;

// Workaround for VS2017 & VS 16.9.x
#if defined(_MSC_VER) && (_MSC_VER < 1920 || _MSC_VER == 1928)

template<std::size_t rank, std::size_t... sizes>
struct GetRankSize
{
  static constexpr std::size_t size_array[] = {sizes...};
  static constexpr std::size_t value = size_array[rank - 1];
};

template<typename T, std::size_t rank, std::size_t... sizes>
using GetArrayImplType =
  std::array<std::conditional_t<rank == 1, T, DimensionalArrayExplicitRank<T, rank - 1, sizes...>>,
             GetRankSize<rank, sizes...>::value>;

#else

template<std::size_t rank, std::size_t... sizes>
constexpr std::size_t GetRankSize()
{
  constexpr std::size_t size_array[] = {sizes...};
  return size_array[rank - 1];
}

template<typename T, std::size_t rank, std::size_t... sizes>
using GetArrayImplType =
  std::array<std::conditional_t<rank == 1, T, DimensionalArrayExplicitRank<T, rank - 1, sizes...>>,
             GetRankSize<rank, sizes...>()>;

#endif

template<typename T, std::size_t rank_param, std::size_t... sizes>
struct DimensionalArrayExplicitRank : public GetArrayImplType<T, rank_param, sizes...>
{
  static constexpr std::size_t rank = rank_param;
  static_assert(rank > 0, "Attempted to create dimensional array with rank less than 1");
  using ArrayImplType = GetArrayImplType<T, rank, sizes...>;
  using ArrayImplType::ArrayImplType;

  template<typename Callable>
  void enumerate(const Callable& f)
  {
    for (auto& it : *this)
    {
      if constexpr (rank == 1)
        f(it);
      else
        it.enumerate(f);
    }
  }

  template<typename Callable>
  void enumerate(const Callable& f) const
  {
    for (const auto& it : *this)
    {
      if constexpr (rank == 1)
        f(it);
      else
        it.enumerate(f);
    }
  }
};
} // namespace detail

template<typename T, std::size_t... sizes>
using DimensionalArray = detail::DimensionalArrayExplicitRank<T, sizeof...(sizes), sizes...>;
