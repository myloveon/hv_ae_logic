/**
****************************************************************************
* @file     ae_camera_interface.hpp
* @brief    AE 가 카메라에 촬영을 요청할 때 쓰는 인터페이스.
*
* @details
*     AEManager 는 "이 노출로 찍어서 밝기를 재 오라"고 요청만 하고,
*     실제 SpaceWire TC(222,1) / TC(222,4) 송수신은
*     이 인터페이스를 구현한 쪽이 담당한다.\n
*     \n
*     덕분에 진짜 카메라 없이도(가짜 카메라로) 알고리즘을 시험할 수 있고,
*     통신 방식이 바뀌어도 AE 본체는 손댈 필요가 없다.
*
* @note     반환값이 "밝기 그리드"가 아니라 "존 통계"인 이유는 두 가지다.\n
*           첫째, AE 피드백은 HDR Merge 이전의 raw 값이어야 한다.\n
*           둘째, 존 통계에는 포화/암부 픽셀 개수가 함께 담겨 있어
*           씬 판정에 필요한 정보를 한 번에 얻을 수 있다.
* @see      ae_scene_classifier.hpp
* @see      ae_zone_stats.cu
****************************************************************************
*/

#pragma once
#include <array>
#include <cstdint>
#include "ae_types.hpp"
#include "ae_scene_classifier.hpp"

namespace ae {

/**
****************************************************************************
* @brief    한 번 촬영한 결과.
****************************************************************************
*/
struct CaptureResult
{
  std::array<ZoneStatsHost, ZONE_COUNT_S> zones{};	/**< 144개 존별 통계 배열 */
  int32_t sensor_temp_c_q8 = 0; 	  	/**< HK Report 의 SENSOR_TEMP (Q8 섭씨) */
  bool success = false;           		/**< 촬영 성공 여부. FID_SENSOR_SYNC 등 오류 시 false */
};

/**
****************************************************************************
* @brief    카메라 촬영 창구 (추상 인터페이스).
*
* @details
*     실제 구현체가 해야 할 일:\n
*       1) TC(222,1) Take Image     - HDR=1, BLACK_COL_EN=1\n
*       2) TC(222,4) Download Image - BLACK_COL_CORR=1, ROI 256x256\n
*       3) ae_zone_stats.cu 커널 실행 -> 존별 통계 산출\n
*       4) TM(3,25) HK Report 에서 SENSOR_TEMP 취득
****************************************************************************
*/
class ICameraCaptureInterface {
 public:
  virtual ~ICameraCaptureInterface() = default;

  /**
  ****************************************************************************
  * @details  지정한 노출로 한 장 찍고 존별 통계를 돌려준다.
  *
  * @param[in] exposure  적용할 긴 노출 / 짧은 노출 레지스터 쌍.
  *
  * @return   CaptureResult : 존 통계와 센서 온도. 실패 시 success 가 false.
  * @warning  구현체는 촬영 실패를 예외가 아니라 success=false 로 알려야 한다.
  *           AE 는 연속 실패 횟수를 세어 세션을 중단한다.
  ****************************************************************************
  */
  virtual CaptureResult capture(const ExposurePair& exposure) = 0;

  /**
  ****************************************************************************
  * @details  현재 시각을 돌려준다. 웜스타트 캐시 만료 판정에 쓰인다.
  *
  * @return   uint64_t : 현재 시각 (ms).
  * @note     구현체가 LOBT 등 시스템 시계를 제공한다.
  ****************************************************************************
  */
  virtual uint64_t now_ms() = 0;
};

}  // namespace ae
