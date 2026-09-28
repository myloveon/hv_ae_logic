/**
****************************************************************************
* @file     ae_black_level.hpp
* @brief    블랙레벨 오프셋 잔차 보정 (정수 전용).
*
* @details
*     설계 원칙은 3단계다.\n
*     \n
*     1) 카메라 자체의 실시간 라인단위 보정을 항상 켜둔다.\n
*        (BLACK_COL_EN=1 촬영 + BLACK_COL_CORR=1 다운로드)\n
*        이 보정은 각 라인의 첫 16픽셀(차광 레퍼런스)을 같은 노출시간 동안
*        적분해 빼주므로, 다크커런트의 노출시간 의존성까지 자동 포함된다.\n
*        또한 라인 단위라 HDR 의 짝수행(long) / 홀수행(short) 이
*        각자의 노출에 맞는 오프셋을 개별 차감받는다.
*        그래서 다운로드된 데이터에서는 오프셋이 거의 0 이 된다.\n
*     \n
*     2) 그래도 남을 수 있는 "잔차"만 지상 캘리브레이션으로
*        온도별 선형모델(r0 + r1 x t)로 특성화해 둔다.\n
*     \n
*     3) 잔차가 데드밴드보다 훨씬 작으면 이 모듈을 비활성으로 두고
*        항상 0 을 반환하게 한다.
*
* @note     기본값은 비활성이다. 카메라 내장 보정만 신뢰한다는 뜻이다.
* @note     활성화 여부는 EGSE 실측(렌즈캡 다크 프레임)으로 결정해야 한다.
* @see      ae_manager_v2.hpp
****************************************************************************
*/

#pragma once
#include <cstdint>
#include "ae_fixed.hpp"

namespace ae {

/**
****************************************************************************
* @brief    온도 구간 하나에 대한 잔차 선형계수.
*
* @details
*     이 구간에서의 잔차는 다음 식으로 계산된다.\n
*       residual_q8 = r0_q8 + (r1_q16 x exposure_us) >> 16
****************************************************************************
*/
struct BlackLevelTempBin
{
  int32_t  temp_lo_c_q8;   /**< 이 구간이 적용되는 온도 하한 (Q8 섭씨) */
  int32_t  temp_hi_c_q8;   /**< 이 구간이 적용되는 온도 상한 (Q8 섭씨) */
  uint32_t r0_q8;          /**< 노출이 0일 때의 잔차 (Q8 밝기) */
  uint32_t r1_q16;         /**< 노출시간(us)에 비례하는 잔차 기울기 (Q16) */
};

/**
****************************************************************************
* @brief    온도와 노출시간으로 블랙레벨 잔차를 추정하는 모델.
*
* @details
*     지상 캘리브레이션으로 구한 온도별 계수표를 load_calibration() 으로
*     넣어두고, 매 촬영마다 residual_q8() 으로 뺄 값을 구한다.
****************************************************************************
*/
class BlackLevelModel
{
 public:
  /**
  ****************************************************************************
  * @details  지상 캘리브레이션 결과를 등록한다.
  *
  * @param[in] bins     온도 구간별 계수표의 시작 주소.
  * @param[in] count    계수표 항목 개수.
  * @param[in] enabled  이 모델을 사용할지 여부.
  *
  * @return   void
  * @note     캘리브레이션 결과 "잔차가 무시할 만하다"고 나오면
  *           enabled 를 false 로 두면 된다.
  ****************************************************************************
  */
  void load_calibration(const BlackLevelTempBin* bins, uint32_t count,
                        bool enabled)
  {
    bins_ = bins; count_ = count; enabled_ = enabled;
  }

  /**
  ****************************************************************************
  * @details  현재 온도와 노출시간에 해당하는 잔차 오프셋을 계산한다.
  *
  * @param[in] temp_c_q8    현재 센서 온도 (Q8 섭씨).
  * @param[in] exposure_us  현재 노출시간 (마이크로초, 정수).
  *
  * @return   uint32_t : 측정 밝기에서 빼야 할 잔차 (Q8 밝기).
  *                      모델이 비활성이거나 계수표가 없으면 0.
  * @note     온도가 어느 구간에도 안 들어가면 가장 가까운 구간을 쓴다.
  ****************************************************************************
  */
  uint32_t residual_q8(int32_t temp_c_q8, uint32_t exposure_us) const
  {
    if (!enabled_ || count_ == 0 || bins_ == nullptr) return 0;

	const BlackLevelTempBin* b = find_bin(temp_c_q8);

	if (b == nullptr)
		b = nearest_bin(temp_c_q8);

    const uint64_t slope =
        (static_cast<uint64_t>(b->r1_q16) * exposure_us) >> fx::kQ16;

    return b->r0_q8 + static_cast<uint32_t>(slope);
  }

  /**
  ****************************************************************************
  * @details  이 모델이 활성 상태인지 알려준다.
  *
  * @return   bool : 활성이면 true.
  ****************************************************************************
  */
  bool is_enabled() const { return enabled_; }

 private:
  /**
  ****************************************************************************
  * @details  주어진 온도가 포함되는 구간을 찾는다.
  *
  * @param[in] t  찾을 온도 (Q8 섭씨).
  *
  * @return   const BlackLevelTempBin* : 해당 구간. 없으면 nullptr.
  ****************************************************************************
  */
  const BlackLevelTempBin* find_bin(int32_t t) const
  {
    for (uint32_t i = 0; i < count_; ++i)
	{
      if (t >= bins_[i].temp_lo_c_q8 && t <= bins_[i].temp_hi_c_q8)
	  	return &bins_[i];
    }
    return nullptr;
  }

  /**
  ****************************************************************************
  * @details  구간 중심값이 주어진 온도에 가장 가까운 구간을 찾는다.
  *
  * @param[in] t  찾을 온도 (Q8 섭씨).
  *
  * @return   const BlackLevelTempBin* : 가장 가까운 구간.
  * @pre      count_ 가 1 이상이어야 한다.
  ****************************************************************************
  */
  const BlackLevelTempBin* nearest_bin(int32_t t) const
  {
    const BlackLevelTempBin* best = &bins_[0];
    uint32_t best_d = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < count_; ++i)
	{
      const int32_t c = (bins_[i].temp_lo_c_q8 + bins_[i].temp_hi_c_q8) / 2;
      const uint32_t d = static_cast<uint32_t>((c > t) ? (c - t) : (t - c));

	  if (d < best_d)
	  	{ best_d = d; best = &bins_[i]; }
    }
    return best;
  }

  const BlackLevelTempBin* bins_ = nullptr;  /**< 온도 구간별 계수표 */
  uint32_t count_ = 0;                       /**< 계수표 항목 개수 */
  bool enabled_ = false;                     /**< 기본 비활성 (카메라 내장 보정만 신뢰) */
};

}  // namespace ae
