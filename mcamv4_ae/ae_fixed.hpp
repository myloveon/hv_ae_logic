// ae_fixed.hpp
//
// AE 모듈 전체가 공유하는 고정소수점(정수 전용) 연산 헬퍼.
//
// ── 코딩 규칙 ──────────────────────────────────────────────
//   금지: float / double / exp() / pow() / sqrt() / log() / 소수 리터럴
//   사용: uint16_t, uint32_t, (필요시) uint64_t 중간연산,
//         Q-format, LUT, Multiply + Shift, 상수분수는 Multiply/Divide
//   대체 불가능할 때만 예외 허용.
//
// ── 도메인별 표현 규약 ─────────────────────────────────────
//   밝기      : Q8  (bri_q8)   센서 12bit(0~4095) x 256 = 최대 1,048,320  -> uint32
//   노출시간  : Q8  us (us_q8) 최대 104,000us x 256 = 26,624,000          -> uint32
//   비율/가중 : Q16 (ratio_q16) 최대 31.0 -> 2,031,616                    -> uint32
//   중간 연산 : uint64_t (곱셈 오버플로 방지)
//
// ── 나눗셈에 대한 판단 ─────────────────────────────────────
//   HDR Merge 커널(GPU, 픽셀당 수백만 회)에서는 나눗셈을 reciprocal LUT +
//   Multiply/Shift로 전부 대체했다. 반면 AE는 **호스트에서 초당 5회** 수준으로만
//   실행되므로, 변수 분모 나눗셈을 정수 나눗셈으로 두는 편이
//   (a) 정확도가 높고 (b) LUT 메모리/생성 단계가 불필요하다.
//   따라서 이 파일은 정수 나눗셈을 기본으로 하되, GPU 이식이 필요할 때를 대비해
//   reciprocal 경로(recip_q30 / mul_recip_q30)도 함께 제공한다.
//   어느 쪽이든 float/exp/pow는 사용하지 않는다.

#pragma once
#include <cstdint>

namespace ae {
namespace fx {

// ============================================================
// Q-format 상수
// ============================================================
constexpr int      kQ8       = 8;
constexpr int      kQ16      = 16;
constexpr int      kQ30      = 30;
constexpr uint32_t kOne_q8   = 1u << kQ8;    // 256
constexpr uint32_t kOne_q16  = 1u << kQ16;   // 65536

// ============================================================
// 사람이 읽기 쉬운 상수 분수 표현
// ============================================================
// 튜닝 파라미터처럼 값이 고정된 비율은 Q16 숫자를 직접 노출하지 않고
// 7/10, 3/5, 9/10처럼 의미가 보이게 표현한다.
struct Fraction
{
  uint32_t num;
  uint32_t den;
};

constexpr Fraction fraction(uint32_t num, uint32_t den)
{
  return Fraction{num, (den == 0u) ? 1u : den};
}

constexpr Fraction percent(uint32_t value)
{
  return fraction(value, 100u);
}

constexpr uint32_t q16_ratio(uint32_t num, uint32_t den)
{
  return (den == 0u)
      ? 0u
      : static_cast<uint32_t>((static_cast<uint64_t>(num) << kQ16) / den);
}

constexpr uint32_t q16_percent(uint32_t value)
{
  return q16_ratio(value, 100u);
}

inline uint32_t apply_fraction(uint32_t value, Fraction f)
{
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(value) * f.num) / f.den);
}

// part / whole >= threshold 를 나눗셈 없이 판정한다.
inline bool fraction_at_least(uint64_t part, uint64_t whole, Fraction threshold)
{
  if (whole == 0u)
  	return false;

  return (part * threshold.den >= whole * threshold.num);
}

// part / whole < threshold 를 나눗셈 없이 판정한다.
inline bool fraction_less_than(uint64_t part, uint64_t whole, Fraction threshold)
{
  if (whole == 0u)
  	return false;

  return part * threshold.den < whole * threshold.num;
}

// ============================================================
// 기본 산술 (전부 정수)
// ============================================================

// a(Q16) x b(Q16) -> Q16   (Multiply + Shift)
inline uint32_t mul_q16(uint32_t a_q16, uint32_t b_q16)
{
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(a_q16) * b_q16) >> kQ16);
}

// value x ratio(Q16) -> value 스케일 유지 (Multiply + Shift)
inline uint32_t scale_q16(uint32_t value, uint32_t ratio_q16)
{
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(value) * ratio_q16) >> kQ16);		//(value X ratioQ16​​) / 65,536
}

// value / ratio(Q16) -> value 스케일 유지
inline uint32_t unscale_q16(uint32_t value, uint32_t ratio_q16)
{
  if (ratio_q16 == 0) return value;
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(value) << kQ16) / ratio_q16);		//(value X 65,536) / ratioQ16​​
}

// num / den 을 Q16 비율로 (0 분모 안전)
inline uint32_t ratio_q16(uint64_t num, uint64_t den)
{
  if (den == 0) return 0;
  return static_cast<uint32_t>((num << kQ16) / den);
}

// 반올림 정수 나눗셈
inline uint64_t udiv_round(uint64_t num, uint64_t den)
{
  if (den == 0) return 0;
  return (num + (den >> 1)) / den;
}

// 부호 없는 두 값의 절대 차
inline uint32_t abs_diff(uint32_t a, uint32_t b)
{
  return (a > b) ? (a - b) : (b - a);
}

// 정수 clamp
inline uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
  return (v < lo) ? lo : ((v > hi) ? hi : v);
}

// ============================================================
// 정수 제곱근 (Newton, float/sqrt 미사용)
//   기하중앙(로그 축 중앙) 계산에 사용한다.
// T-center = Sqrt(T-min * T-max)
// ============================================================
inline uint32_t isqrt64(uint64_t x)
{
  if (x == 0) return 0;
  if (x < 4) return 1;

  // 초기 추정값: 최상위 비트 위치의 절반 (Shift만 사용)
  int b = 63;
  while (b > 0 && ((x >> b) & 1ull) == 0ull) --b;

  uint64_t r = 1ull << ((b >> 1) + 1);

  // Newton 반복: r = (r + x/r) / 2  (정수 나눗셈)
  uint64_t prev;
  do
  {
    prev = r;
    r = (r + x / r) >> 1;
  } while (r < prev);

  return static_cast<uint32_t>(prev);
}

// 두 값의 기하평균 = sqrt(a*b)
inline uint32_t geometric_mean(uint32_t a, uint32_t b)
{
  return isqrt64(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
}

// ============================================================
// 로그 비교 대체 (log() 미사용)
//
//   "후보 r 중 ideal에 로그적으로 가장 가까운 것"을 고를 때,
//   log를 쓰지 않고 기하평균 비교로 동일한 결과를 얻는다.
//
//   단조증가 후보 r_i <= ideal <= r_{i+1} 에 대해
//     |log(ideal) - log(r_i)| < |log(r_{i+1}) - log(ideal)|
//   <=> ideal^2 < r_i * r_{i+1}
//   이므로 곱셈 비교만으로 판정 가능하다.
// ============================================================
// true면 lo 쪽이 로그적으로 더 가깝다.
inline bool closer_in_log(uint32_t ideal_q16, uint32_t lo, uint32_t hi)
{
  const uint64_t lhs = static_cast<uint64_t>(ideal_q16) * ideal_q16;  // Q32
  const uint64_t rhs = (static_cast<uint64_t>(lo) * hi) << 32;        // Q32
  return lhs < rhs;
}

// ============================================================
// (선택) reciprocal 경로 - GPU 이식용
//   정규화 만티사 기반 1/den 근사. HDR Merge 커널과 동일 방식.
//   호스트 AE에서는 기본적으로 쓰지 않는다(정수 나눗셈이 더 정확).
// ============================================================
inline uint32_t msb_pos_u32(uint32_t x)
{
  if (x == 0) return 0;
  uint32_t m = 0;
  while (x >>= 1) ++m;
  return m;
}

inline uint32_t recip_q30(uint32_t den)
{
  if (den < 1u) den = 1u;
  const int m = static_cast<int>(msb_pos_u32(den));
  const int shift = m - 10;  // 11bit 정규화 윈도우 [1024, 2047]
  uint32_t normalized =
      (shift >= 0) ? (den >> shift) : (den << (-shift));
  normalized = clamp_u32(normalized, 1024u, 2047u);
  // (1<<30)/normalized : 분모가 1024~2047로 한정된 정수 나눗셈
  const uint32_t base = static_cast<uint32_t>((1ull << kQ30) / normalized);
  return (shift >= 0) ? (base >> shift) : (base << (-shift));
}

inline uint32_t mul_recip_q30(uint64_t num, uint32_t recip)
{
  return static_cast<uint32_t>((num * recip) >> kQ30);
}

}  // namespace fx
}  // namespace ae
