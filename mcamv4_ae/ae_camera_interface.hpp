// ae_camera_interface.hpp
//
// 카메라 촬영 인터페이스 (정수 전용).
//
// AEManager는 "촬영해서 밝기를 재 오라"고 요청만 하고, 실제 SpaceWire
// TC(222,1)/TC(222,4) 송수신은 이 인터페이스의 구현체가 담당한다.
//
// 반환값이 "밝기 그리드"가 아니라 "존 통계"인 이유:
//   AE 피드백은 HDR Merge 이전의 raw 값이어야 한다. 또한 존 통계에는
//   포화/암부 픽셀 개수가 함께 담겨 있어 씬 판정에 필요한 정보를 한 번에 얻는다.

#pragma once
#include <array>
#include <cstdint>
#include "ae_types.hpp"
#include "ae_scene_classifier.hpp"

namespace ae {

struct CaptureResult
{
  std::array<ZoneStatsHost, ZONE_COUNT_S> zones{};		//144개 Zone 통계 배열
  int32_t sensor_temp_c_q8 = 0; 	  	// HK Report의 SENSOR_TEMP (Q8 섭씨)
  bool success = false;           		// FID_SENSOR_SYNC 등 오류 시 false
};

class ICameraCaptureInterface {
 public:
  virtual ~ICameraCaptureInterface() = default;

  // 구현체가 해야 할 일:
  //   1) TC(222,1) Take Image     - HDR=1, BLACK_COL_EN=1
  //   2) TC(222,4) Download Image - BLACK_COL_CORR=1, ROI 256x256
  //   3) ae_zone_stats.cu 커널 실행 -> 존별 통계
  //   4) TM(3,25) HK Report에서 SENSOR_TEMP 취득
  virtual CaptureResult capture(const ExposurePair& exposure) = 0;

  // 현재 시각 (ms). 웜스타트 캐시 만료 판정에 사용.
  // 구현체가 LOBT 등 시스템 시계를 제공한다.
  virtual uint64_t now_ms() = 0;
};

}  // namespace ae
