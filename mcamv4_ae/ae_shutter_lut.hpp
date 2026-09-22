// ae_shutter_lut.hpp
//
// 셔터 레지스터 코드 <-> 정규화 노출 가중치(Q7) 변환. 정수 전용.
//
// 원본 gnAeShtTbl은 코드 14 증가마다 노출량이 약 x1.1631 되는 로그 인코딩이다.
// 테이블 Y값은 Q7 고정소수점(실값 = linear_q7 / 128)이므로,
// 별도 스케일 변환 없이 Q7 그대로 다룬다.

#pragma once
#include <cstdint>
#include <cstddef>
#include "ae_fixed.hpp"

namespace ae {

struct ShutterTableEntry
{
  uint32_t code;       // 셔터 레지스터 코드값 (X)
  uint32_t linear_q7;  // 정규화 노출 가중치, Q7 (Y)
};

class ShutterLUT
{
 public:
  ShutterLUT(const ShutterTableEntry* table, size_t count)
      : table_(table), count_(count) {}

  // ---- 코드 -> 정규화 가중치(Q7) ----
  uint32_t code_to_q7(uint32_t code) const
  {//table_ => kDefaultShutterTable
    if (count_ == 0)
		return 0;

    if (code <= table_[0].code)
		return table_[0].linear_q7;

    if (code >= table_[count_ - 1].code)
		return table_[count_ - 1].linear_q7;

    const size_t i = bracket_by_code(code);

	//table_ => kDefaultShutterTable
    return interp1d(code, table_[i].code, table_[i + 1].code,
                    table_[i].linear_q7, table_[i + 1].linear_q7);
  }

  // ---- 정규화 가중치(Q7) -> 코드 ----
  uint32_t q7_to_code(uint32_t target_q7) const
  {
    if (count_ == 0)
		return 0;

    if (target_q7 <= table_[0].linear_q7)
		return table_[0].code;

    if (target_q7 >= table_[count_ - 1].linear_q7)
		return table_[count_ - 1].code;

    const size_t i = bracket_by_q7(target_q7);

	//table_ => kDefaultShutterTable
    return interp1d(target_q7, table_[i].linear_q7, table_[i + 1].linear_q7,
                    table_[i].code, table_[i + 1].code);
  }

  uint32_t max_q7() const
  {
    return (count_ > 0) ? table_[count_ - 1].linear_q7 : 0;
  }

  uint32_t max_code() const
  {
    return (count_ > 0) ? table_[count_ - 1].code : 0;
  }

 private:
  // 원본 hv2A_interp1D와 동일한 선형보간.
  // Q7 값이 크므로 오버플로 방지를 위해 uint64_t 중간연산을 사용한다.
  static uint32_t interp1d(uint32_t x, uint32_t x1, uint32_t x2,
                            uint32_t y1, uint32_t y2)
  {
    if (x1 == x2) return y1;
    if (y1 == y2) return y1;

    const uint64_t dx  = static_cast<uint64_t>(x2) - x1;
    uint64_t result;
    if (y1 < y2)
	{
      result = (static_cast<uint64_t>(y2 - y1) *
                (static_cast<uint64_t>(x) - x1)) / dx + y1;
    }
	else
	{
      result = (static_cast<uint64_t>(y1 - y2) *
                (static_cast<uint64_t>(x2) - x)) / dx + y2;
    }
    return static_cast<uint32_t>(result);
  }

  // 코드 간격이 균일하지 않으므로(714~728 구간 예외) 반드시 탐색해야 한다.
  size_t bracket_by_code(uint32_t code) const
  {
    size_t lo = 0, hi = count_ - 1;	//0 ~ 72
    while (hi - lo > 1)			//인덱스 중간 항목이 없기 때문
	{
      const size_t mid = (lo + hi) >> 1;	//2진탐색

      if (table_[mid].code <= code)
	  	lo = mid;
	  else hi = mid;
    }
    return lo;
  }

  size_t bracket_by_q7(uint32_t q7) const
  {
    size_t lo = 0, hi = count_ - 1;  //0 ~ 72
    while (hi - lo > 1)
	{
      const size_t mid = (lo + hi) >> 1;	//2진탐색

      if (table_[mid].linear_q7 <= q7)
	  	lo = mid;
	  else hi = mid;
    }
    return lo;
  }

  const ShutterTableEntry* table_;		//kDefaultShutterTable
  size_t count_;						//kDefaultShutterTableCount
};

}  // namespace ae
