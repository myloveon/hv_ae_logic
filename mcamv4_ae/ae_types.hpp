// ae_types.hpp
//
// AE 공용 타입/상수.
// 알고리즘 본문에서는 물리 의미가 먼저 보이도록 별칭을 사용하고,
// 실제 저장 형식(Q8/Q16)은 이 파일에만 모아 둔다.

#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include "ae_fixed.hpp"

namespace ae {

// ============================================================
// 1. MCAMv4 ICD 기반 상수
// ============================================================
using Brightness   = uint32_t;  // Q8, 12bit sensor code x 256
using ExposureUs   = uint32_t;  // Q8 microsecond
using Ratio        = uint32_t;  // Q16
using TemperatureC = int32_t;   // Q8 degree C

// EXPOSURE1/2 필드는 0~32767 (15bit) - ICD Table 36
constexpr uint16_t EXPOSURE_MIN = 0;
constexpr uint16_t EXPOSURE_MAX = 32767; 	// 2^15 - 1

// EXPOSURE_STEP: 0 = 10us, 1 = 1ms - ICD Table 37
enum class ExposureStep : uint8_t
{
  kStep10us = 0,					//kStep10us = 10u * fx::kOne_q8;
  kStep1ms  = 1,					//kStep1ms  = 1000u * fx::kOne_q8;
};

// 스텝 크기를 Q8 마이크로초로 (10us -> 2560, 1ms -> 256000)
constexpr ExposureUs kStep10us = 10u * fx::kOne_q8;
constexpr ExposureUs kStep1ms  = 1000u * fx::kOne_q8;

inline ExposureUs step_size(ExposureStep step)
{
  return (step == ExposureStep::kStep10us) ? kStep10us : kStep1ms;
}

// 센서 픽셀 최대값 (12bpp)
constexpr uint16_t PIXEL_MAX_VALUE = 4095;
// 밝기를 Q8로 표현했을 때의 최대값
constexpr Brightness PIXEL_MAX = static_cast<Brightness>(PIXEL_MAX_VALUE) * fx::kOne_q8;

// 센서 고정 오버헤드 (ICD: 실제 노출 = sensor_exposure + 33us)
constexpr ExposureUs kExposureOverhead = 33u * fx::kOne_q8;

// ============================================================
// 2. AE 알고리즘 파라미터 (전부 정수/Q16)
// ============================================================
struct AEAlgoParams
{
  // 최대 반복 횟수 (5fps -> 세션당 5샷 예산)
  int max_iterations						 = 5;

  // 데드밴드: 목표밝기 대비 이 비율 이내면 수렴 판정 (Q16)
  Ratio tolerance							 = fx::q16_percent(4);  // 4%
  // 1회 조정에서 허용하는 최대 노출 변화 배율 (Q16)
  Ratio max_step_ratio						 = fx::q16_ratio(2, 1); // x2

  // 유효 존 최소 개수 (144존 중)
  int min_valid_zones						 = 30;
  // 존 값이 "포화"로 간주되는 임계 (12bit 원본 스케일)
  uint16_t saturation_threshold				 = 4000;				//<-- Saturation Limite
  // 존 값이 "암부 클리핑"으로 간주되는 임계
  uint16_t dark_clip_threshold				 = 32;					//<-- Clip
};

// ============================================================
// 3. 12x12 존 그리드 상수
// ============================================================
constexpr int ZONE_GRID_W = 12;
constexpr int ZONE_GRID_H = 12;
constexpr int ZONE_COUNT  = ZONE_GRID_W * ZONE_GRID_H;   // 144

// ============================================================
// 4. 노출 파라미터 쌍 (EXPOSURE1 = long, EXPOSURE2 = short)
// ============================================================
struct ExposurePair
{
  uint16_t exposure1 = 0;  // long
  uint16_t exposure2 = 0;  // short
  ExposureStep step = ExposureStep::kStep10us;

  // 실제 물리 노출시간을 Q8 마이크로초로 환산 (Multiply 만 사용)
  //   sensor_exposure = EXPOSURE_STEP x EXPOSUREn,  실제 = + 33us
  ExposureUs long_effective_time() const
  {
    return exposure1 * step_size(step) + kExposureOverhead;
  }

  ExposureUs short_effective_time() const
  {
    return exposure2 * step_size(step) + kExposureOverhead;
  }
};

}  // namespace ae
