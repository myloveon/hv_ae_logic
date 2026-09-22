// ae_black_level.hpp
//
// 블랙레벨 오프셋(b) 잔차 보정. 정수 전용.
//
// ── 설계 원칙 ──────────────────────────────────────────────
//  1) 1차적으로 카메라 자체의 실시간 라인단위 보정
//     (BLACK_COL_EN=1 촬영 + BLACK_COL_CORR=1 다운로드)을 항상 켜둔다.
//     이 보정은 각 라인의 첫 16픽셀(차광 레퍼런스)을 같은 노출시간 동안
//     적분해 빼주므로, 다크커런트의 노출시간 의존성까지 자동 포함된다.
//     또한 라인 단위라 HDR의 짝수행(long)/홀수행(short)이 각자의 노출에 맞는
//     오프셋을 개별 차감받는다. -> 다운로드 데이터에서 b ~= 0.
//  2) 완벽하지 않을 수 있는 "잔차"만 지상 캘리브레이션으로 온도별 선형모델
//     (r0 + r1 x t)로 특성화해 둔다.
//  3) 잔차가 데드밴드보다 훨씬 작으면 이 모듈을 비활성(0 반환)으로 둔다.

#pragma once
#include <cstdint>
#include "ae_fixed.hpp"

namespace ae {

// 온도 구간별 잔차 선형계수
//   residual_q8 = r0_q8 + (r1_q16 x exposure_us) >> 16
struct BlackLevelTempBin
{
  int32_t  temp_lo_c_q8;   // 적용 온도 하한 (Q8 섭씨)
  int32_t  temp_hi_c_q8;   // 적용 온도 상한
  uint32_t r0_q8;          // 노출 0일 때의 잔차 (Q8 밝기)
  uint32_t r1_q16;         // 노출시간(us)에 비례하는 잔차 기울기 (Q16)
};

class BlackLevelModel
{
 public:
  // 캘리브레이션 결과 "잔차 무시 가능"이면 enabled=false로 두면 된다.
  void load_calibration(const BlackLevelTempBin* bins, uint32_t count,
                        bool enabled)
  {
    bins_ = bins; count_ = count; enabled_ = enabled;
  }

  // 잔차 오프셋 (Q8 밝기). 비활성이면 항상 0.
  uint32_t residual_q8(int32_t temp_c_q8, uint32_t exposure_us) const
  {
    if (!enabled_ || count_ == 0 || bins_ == nullptr) return 0;

	const BlackLevelTempBin* b = find_bin(temp_c_q8);

	if (b == nullptr) b = nearest_bin(temp_c_q8);

    const uint64_t slope =
        (static_cast<uint64_t>(b->r1_q16) * exposure_us) >> fx::kQ16;

    return b->r0_q8 + static_cast<uint32_t>(slope);
  }

  bool is_enabled() const { return enabled_; }

 private:
  const BlackLevelTempBin* find_bin(int32_t t) const
  {
    for (uint32_t i = 0; i < count_; ++i)
	{
      if (t >= bins_[i].temp_lo_c_q8 && t <= bins_[i].temp_hi_c_q8)
	  	return &bins_[i];
    }
    return nullptr;
  }
  const BlackLevelTempBin* nearest_bin(int32_t t) const
  {
    const BlackLevelTempBin* best = &bins_[0];
    uint32_t best_d = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < count_; ++i)
	{
      const int32_t c = (bins_[i].temp_lo_c_q8 + bins_[i].temp_hi_c_q8) / 2;
      const uint32_t d = static_cast<uint32_t>((c > t) ? (c - t) : (t - c));

	  if (d < best_d) { best_d = d; best = &bins_[i]; }
    }
    return best;
  }

  const BlackLevelTempBin* bins_ = nullptr;
  uint32_t count_ = 0;
  bool enabled_ = false;   // 기본: 비활성 (카메라 내장 보정만 신뢰)
};

}  // namespace ae
