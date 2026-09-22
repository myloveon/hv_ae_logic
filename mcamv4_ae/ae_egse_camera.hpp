// ae_egse_camera.hpp
//
// ┌──────────────────────────────────────────────────────────────┐
// │ 이 파일이 하는 일                                              │
// │   EGSE(지상시험장비)로 실측할 때 쓰는 카메라 창구를 정의한다.    │
// │                                                              │
// │   평소 AE는 "노출값"만 정해서 찍는다.                          │
// │   그런데 실측할 때는 HDR을 끄거나, 12bpp로 받거나,             │
// │   블랙레벨 보정을 꺼보는 등 여러 설정을 바꿔가며 찍어야 한다.    │
// │   그래서 설정을 직접 지정할 수 있는 창구를 따로 둔다.           │
// └──────────────────────────────────────────────────────────────┘
//
// 이 파일은 AE_ENABLE_EGSE_TOOLS 가 정의됐을 때만 쓰인다.
// 비행 소프트웨어에는 포함되지 않는다.

#pragma once

#ifdef AE_ENABLE_EGSE_TOOLS

#include <array>
#include <cstdint>
#include "ae_types.hpp"
#include "ae_scene_classifier.hpp"

namespace ae {
namespace egse {

// ==============================================================
// 카메라 다운로드 포맷 (ICD Table 52)
//   센서 자체는 12bpp인데, 내려받을 때 몇 비트로 받을지 고를 수 있다.
// ==============================================================
enum class DataFormat : uint8_t {
  kRaw8bpp  = 0,   // 1 byte / 픽셀      (최대 255)
  kRaw10bpp = 1,   // 5 byte / 4픽셀     (최대 1023)
  kRaw12bpp = 2,   // 3 byte / 2픽셀     (최대 4095)
};

// 이 포맷에서 픽셀이 가질 수 있는 최대값
inline uint32_t format_pixel_max(DataFormat f) {
  switch (f) {
    case DataFormat::kRaw8bpp:  return 255u;
    case DataFormat::kRaw10bpp: return 1023u;
    case DataFormat::kRaw12bpp: return 4095u;
  }
  return 4095u;
}

// ==============================================================
// 한 번 찍을 때 지정하는 모든 설정
//   실제 구현체는 이 값들을 TC(222,1) / TC(222,4) 파라미터로 옮기면 된다.
// ==============================================================
struct ShotSetup {
  // --- TC(222,1) Take Image ---
  bool     hdr_enabled   = true;    // HDR 켤지 (끄면 EXPOSURE1만 사용)
  uint16_t exposure1     = 100;     // 긴 노출 레지스터
  uint16_t exposure2     = 10;      // 짧은 노출 레지스터 (HDR일 때만)
  ExposureStep step      = ExposureStep::kStep10us;
  bool     black_col_en  = true;    // 차광 기준 픽셀 사용
  bool     sensor_rst_cfg = true;   // 센서 재설정 (ICD상 20ms 추가 소요)

  // --- TC(222,4) Download Image ---
  DataFormat data_format = DataFormat::kRaw12bpp;
  bool     black_col_corr = true;   // 차광 기준으로 블랙레벨 자동 보정
  uint16_t roi_size      = 256;     // 정사각 ROI 한 변 (최소 256, 8의 배수)
};

// ==============================================================
// 한 번 찍은 결과
// ==============================================================
struct ShotResult {
  std::array<ZoneStatsHost, ZONE_COUNT_S> zones{};
  int32_t  sensor_temp_c_q8 = 0;    // HK Report의 SENSOR_TEMP

  bool     success = false;
  uint16_t fid = 0;                 // 실패 시 FID 코드 (0xDE0A 등)

  // IMAGE_HEADER에서 되읽은 값.
  // "내가 지시한 값이 정말 그 사진에 적용됐는가"를 확인하는 데 쓴다.
  // (설정이 한 프레임 늦게 반영되는지 잡아내는 용도 - F항목)
  uint16_t header_exposure1 = 0;
  uint16_t header_exposure2 = 0;

  // 실제로 걸린 시간 (ms). 5샷 예산을 계산할 때 쓴다.
  uint32_t elapsed_ms = 0;
};

// ==============================================================
// EGSE 측정용 카메라 창구
//
//   실제 구현이 해야 할 일:
//     1) ShotSetup 대로 TC(222,1) Take Image 발행
//     2) ShotSetup 대로 TC(222,4) Download Image 발행
//     3) 받은 RAW로 존 통계 계산 (ae_zone_stats.cu 커널)
//     4) TM(3,25) HK Report에서 SENSOR_TEMP 읽기
//     5) IMAGE_HEADER의 EXPOSURE1/2를 그대로 되돌려주기
// ==============================================================
class IEgseCamera {
 public:
  virtual ~IEgseCamera() = default;
  virtual ShotResult shoot(const ShotSetup& setup) = 0;
};

}  // namespace egse
}  // namespace ae

#endif  // AE_ENABLE_EGSE_TOOLS
