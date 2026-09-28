/**
****************************************************************************
* @file     ae_warm_start_cache.hpp
* @brief    직전 AE 수렴 결과를 다음 세션 시작점으로 재사용하는 캐시.
*
* @details
*     AE 는 한 세션에 5장밖에 찍을 수 없으므로 시작점이 매우 중요하다.\n
*     \n
*       초기값이 목표와 20배 차이  -> 5샷으로도 미수렴\n
*       초기값이 목표와 1.4배 차이 -> 2샷 만에 정확히 수렴\n
*     \n
*     Secant 계산 자체는 빠르지만 헌팅 방지용 rate limit 때문에
*     초기값이 크게 틀리면 5샷 예산 안에 도달하지 못한다.\n
*     그래서 직전 세션의 수렴 결과를 저장해 두고 다음 세션의 출발점으로 쓴다.
*
* @note     저장하는 것은 숫자 4개뿐이다.
*            수렴한 로그 코드 / 그때의 밝기(Q8) / 센서온도(Q8) / HDR 비율
* @note     조도가 급변하면 오래된 값이 오히려 해로우므로 유효시간을 둔다.
* @see      ae_manager_v2.hpp
****************************************************************************
*/

#pragma once
#include <cstdint>
#include "ae_fixed.hpp"

namespace ae {

/**
****************************************************************************
* @brief    캐시에 저장되는 직전 세션의 수렴 결과.
****************************************************************************
*/
struct WarmStartEntry
{
  uint32_t code			 	 = 0;		/**< 수렴한 Long Exposure 로그 코드 */
  uint32_t brightness_q8	 = 0;		/**< 그때 측정된 밝기 (Q8) */
  int32_t  sensor_temp_c_q8  = 0;		/**< AE 측정 당시의 센서 온도 (Q8 섭씨) */
  uint32_t hdr_ratio		 = 0;		/**< 그때 사용한 Long/Short HDR 비율 (예: 8 이면 8:1) */
  uint64_t timestamp_ms		 = 0;		/**< 캐시를 저장한 시각 (camera_.now_ms() 기준) */
  bool valid				 = false;	/**< 저장된 값이 있는지 여부 */
};

/**
****************************************************************************
* @brief    웜스타트 캐시. 값 하나만 보관하며 유효시간이 지나면 버린다.
*
* @details
*     세션이 끝날 때 update() 로 저장하고,
*     다음 세션 시작 때 get_if_valid() 로 꺼내 쓴다.
****************************************************************************
*/
class WarmStartCache
{
 public:
  /**
  ****************************************************************************
  * @details  캐시 유효시간을 설정하고 빈 캐시 상태로 시작한다.
  *
  * @param[in] max_age_ms  저장된 값을 믿을 수 있는 시간 (ms). 기본 30초.
  *
  * @note     지구 그림자에서 나와 햇빛을 받는 것처럼 조도가 급변하면
  *           옛날 값이 오히려 해로우므로 유효시간을 짧게 둔다.
  ****************************************************************************
  */
  explicit WarmStartCache(uint64_t max_age_ms = 30000) : max_age_ms_(max_age_ms) {}

  /**
  ****************************************************************************
  * @details  다음 세션의 시작점으로 사용할 AE 결과를 저장한다.
  *
  * @param[in] code           수렴한 로그 코드.
  * @param[in] brightness_q8  그때 측정된 밝기 (Q8).
  * @param[in] temp_q8        그때의 센서 온도 (Q8 섭씨).
  * @param[in] hdr_ratio      그때 사용한 HDR 비율.
  * @param[in] now_ms         현재 시각 (ms).
  *
  * @return   void
  * @note     수렴에 실패한 세션에서도 "가장 가까웠던 값"을 저장한다.
  *           그러지 않으면 실패 -> 캐시 없음 -> 또 실패의 악순환에 빠진다.
  ****************************************************************************
  */
  void update(uint32_t code, uint32_t brightness_q8, int32_t temp_q8,
              uint32_t hdr_ratio, uint64_t now_ms)
  {
    entry_ = WarmStartEntry{code, brightness_q8, temp_q8, hdr_ratio, now_ms, true};
  }

  /**
  ****************************************************************************
  * @details  캐시가 아직 유효하면 out 에 복사해 준다.
  *
  * @param[in]  now_ms  현재 시각 (ms).
  * @param[out] out     유효한 경우 여기에 캐시 내용이 채워진다.
  *
  * @return   bool : 유효한 값을 돌려줬으면 true.
  *                  저장된 값이 없거나, 시계가 뒤로 갔거나,
  *                  유효시간이 지났거나, out 이 null 이면 false.
  ****************************************************************************
  */
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

  /**
  ****************************************************************************
  * @details  캐시를 즉시 무효화한다.
  *
  * @return   void
  * @note     자세 급변처럼 "지금 캐시는 못 믿는다"고 외부에서 판단했을 때 호출한다.
  ****************************************************************************
  */
  void invalidate() { entry_.valid = false; }

 private:
  WarmStartEntry entry_;   /**< 보관 중인 직전 세션 결과 */
  uint64_t max_age_ms_;    /**< 캐시 유효시간 (ms) */
};

}  // namespace ae
