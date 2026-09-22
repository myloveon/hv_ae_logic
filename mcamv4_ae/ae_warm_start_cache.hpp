// ae_warm_start_cache.hpp
//
// 웜스타트 캐시 (정수 전용).
//
//   초기값이 목표와 20배 차이  -> 5샷으로도 미수렴
//   초기값이 목표와 1.4배 차이 -> 2샷 만에 정확히 수렴
// Secant 자체는 훌륭하지만, 헌팅 방지용 rate limit 때문에
// 초기값이 크게 틀리면 5샷 예산 안에 도달하지 못한다.
//
// ── 저장하는 것 (숫자 4개뿐) ───────────────────────────────
//   수렴한 로그 코드 / 그때의 밝기(Q8) / 센서온도(Q8) / HDR 비율

#pragma once
#include <cstdint>
#include "ae_fixed.hpp"

namespace ae {

struct WarmStartEntry
{
  uint32_t code			 	 = 0;			//Long Exposure 로그 코드
  uint32_t brightness_q8	 = 0;			//측정된 밝기
  int32_t  sensor_temp_c_q8  = 0;			//AE 측정 당시의 센서 온도
  uint32_t hdr_ratio		 = 0;			//Long/Short HDR 비율::Long:Short = 8:1
  uint64_t timestamp_ms		 = 0;			//캐시를 저장한 시각::camera_.now_ms()
  bool valid				 = false;
};

class WarmStartCache
{
 public:
  // 역할: 캐시 유효시간을 설정하고 빈 캐시로 시작한다.
  explicit WarmStartCache(uint64_t max_age_ms = 30000) : max_age_ms_(max_age_ms) {}

  // 역할: 다음 세션의 시작점으로 사용할 AE 결과를 저장한다.
  void update(uint32_t code, uint32_t brightness_q8, int32_t temp_q8,
              uint32_t hdr_ratio, uint64_t now_ms)
  {
    entry_ = WarmStartEntry{code, brightness_q8, temp_q8, hdr_ratio, now_ms, true};
  }

  // 역할: 캐시가 유효시간 안이면 out에 복사한다.
  // 반환: 성공 true, 만료·시각 역행·null 출력 포인터면 false.
  bool get_if_valid(uint64_t now_ms, WarmStartEntry* out) const
  {
    if (out == nullptr)
		return false;

	if (!entry_.valid)									// No values saved
		return false;

    if (now_ms < entry_.timestamp_ms)					// The current time is earlier than the save time
		return false;

    if (now_ms - entry_.timestamp_ms > max_age_ms_)		// 30 seconds have passed since saving
		return false;

    *out = entry_;
    return true;
  }

  // 역할: 장면 급변 등 외부 판단에 따라 캐시를 즉시 무효화한다.
  void invalidate() { entry_.valid = false; }

 private:
  WarmStartEntry entry_;
  uint64_t max_age_ms_;
};

}  // namespace ae
