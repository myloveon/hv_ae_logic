// ae_egse_measure.hpp
//
// ┌──────────────────────────────────────────────────────────────┐
// │ 이 파일이 하는 일                                              │
// │   EGSE로 카메라를 실제로 찍어보면서, AE에 필요한 값들을 잰다.    │
// │                                                              │
// │   AE 코드에는 아직 "확실하지 않은 값"이 몇 개 있다.             │
// │   그 값들을 추측이 아니라 실측으로 정하기 위한 도구다.          │
// └──────────────────────────────────────────────────────────────┘
//
// 측정 항목 (권장 실행 순서)
//
//   A. HDR + 12bpp 호환    - 5분   : 안 되면 이후 계획이 통째로 바뀜
//   B. 선형성 + 33us 검증  - 30분  : AE의 핵심 전제가 맞는지
//   F. 노출 반영 지연      - 15분  : 틀리면 계산 근간이 흔들림
//   C. 블랙레벨 잔차       - 반나절 : 렌즈캡 씌우고
//   D. 목표 밝기 확정      - 1~2시간
//   E. 짧은 노출 목표 확정 - 1시간
//
// 이 파일은 AE_ENABLE_EGSE_TOOLS 가 정의됐을 때만 쓰인다.

#pragma once

#ifdef AE_ENABLE_EGSE_TOOLS

#include <cstdint>
#include <cstdio>
#include <vector>
#include "ae_egse_camera.hpp"
#include "ae_fixed.hpp"
#include "ae_types.hpp"
#include "ae_scene_classifier.hpp"

namespace ae {
namespace egse {

// ==============================================================
// 측정 한 줄의 기록
//   나중에 CSV로 저장해 분석에 쓴다.
// ==============================================================
struct MeasureRow {
  const char* item = "";        // 측정 항목 (A~F)
  uint16_t exposure1 = 0;
  uint16_t exposure2 = 0;
  uint32_t nominal_us = 0;      // 레지스터 x STEP
  uint32_t effective_us = 0;    // 위 + 33us
  uint32_t data_bits = 12;
  bool     hdr = false;
  bool     black_corr = true;

  uint32_t mean_pixel = 0;      // 유효 존 평균 밝기 (픽셀 단위)
  uint32_t saturated_pct = 0;   // 포화 존 비율 (%)
  uint32_t dark_pct = 0;        // 암부 존 비율 (%)
  int32_t  temp_c = 0;

  uint16_t header_exposure1 = 0;
  uint32_t elapsed_ms = 0;
  bool     ok = false;
  uint16_t fid = 0;
};

// ==============================================================
// 존 통계에서 "긴 노출 평균 밝기"를 픽셀 단위로 뽑는다.
//   AE 본체의 compute_weighted_metering()과 같은 기준으로 재야
//   나중에 그 값을 그대로 설정에 쓸 수 있다.
// ==============================================================
inline uint32_t mean_long_pixel(const std::array<ZoneStatsHost, ZONE_COUNT_S>& zones) {
  uint64_t sum = 0;
  uint32_t used = 0;
  for (const ZoneStatsHost& z : zones) {
    if (z.cnt_long == 0u) continue;
    sum += z.acc_long / z.cnt_long;
    ++used;
  }
  return used ? static_cast<uint32_t>(sum / used) : 0u;
}

// 포화/암부 존이 전체의 몇 %인지 (긴 노출 기준)
inline uint32_t saturated_zone_percent(
    const std::array<ZoneStatsHost, ZONE_COUNT_S>& zones, uint32_t pixel_max) {
  // 픽셀 최대값의 98% 이상이면 "포화된 존"으로 센다
  const uint32_t limit = (pixel_max * 98u) / 100u;
  uint32_t hit = 0, used = 0;
  for (const ZoneStatsHost& z : zones) {
    if (z.cnt_long == 0u) continue;
    ++used;
    if ((z.acc_long / z.cnt_long) >= limit) ++hit;
  }
  return used ? (hit * 100u) / used : 0u;
}

inline uint32_t dark_zone_percent(
    const std::array<ZoneStatsHost, ZONE_COUNT_S>& zones, uint32_t pixel_max) {
  // 픽셀 최대값의 1% 이하면 "너무 어두운 존"으로 센다
  const uint32_t limit = (pixel_max * 1u) / 100u;
  uint32_t hit = 0, used = 0;
  for (const ZoneStatsHost& z : zones) {
    if (z.cnt_long == 0u) continue;
    ++used;
    if ((z.acc_long / z.cnt_long) <= limit) ++hit;
  }
  return used ? (hit * 100u) / used : 0u;
}

// ==============================================================
// 측정 실행기
// ==============================================================
class Measurement {
 public:
  explicit Measurement(IEgseCamera& camera) : camera_(camera) {}

  const std::vector<MeasureRow>& rows() const { return rows_; }
  void clear() { rows_.clear(); }

  // ------------------------------------------------------------
  // [A] HDR + 12bpp 호환 확인
  //
  //   왜 제일 먼저 하나?
  //     안 되면 8bpp로 내려받아야 하고, 그러면 AE의 임계값을 전부
  //     다시 계산해야 한다. 이후 모든 측정의 전제가 바뀐다.
  //
  //   판정: FID 0xDE0A (INCOMPATIBLE_PARAM)가 나오면 그 조합은 불가
  // ------------------------------------------------------------
  bool run_A_format_compatibility() {
    std::printf("\n=== [A] HDR + 다운로드 포맷 호환 확인 ===\n");
    std::printf("%-6s %-8s %-6s %-9s %8s %s\n",
                "HDR", "포맷", "ROI", "블랙보정", "결과", "비고");

    const DataFormat formats[] = {DataFormat::kRaw12bpp,
                                   DataFormat::kRaw10bpp,
                                   DataFormat::kRaw8bpp};
    const uint16_t rois[] = {256u, 512u};
    bool hdr_12bpp_ok = false;

    for (bool hdr : {true, false}) {
      for (DataFormat fmt : formats) {
        for (uint16_t roi : rois) {
          ShotSetup s;
          s.hdr_enabled = hdr;
          s.exposure1 = 1000u;
          s.exposure2 = 100u;
          s.data_format = fmt;
          s.roi_size = roi;
          s.black_col_en = true;
          s.black_col_corr = true;

          const ShotResult r = camera_.shoot(s);
          record("A", s, r);

          std::printf("%-6s %-8u %-6u %-9s %8s ",
                      hdr ? "켬" : "끔", format_bits(fmt), roi, "켬",
                      r.success ? "성공" : "실패");
          if (!r.success) {
            std::printf("FID=0x%04X", r.fid);
            if (r.fid == 0xDE0Au) std::printf(" (조합 불가)");
          } else if (hdr && fmt == DataFormat::kRaw12bpp) {
            hdr_12bpp_ok = true;
            std::printf("<- AE 측광에 쓸 조합");
          }
          std::printf("\n");
        }
      }
    }

    std::printf("\n판정: HDR + 12bpp = %s\n",
                hdr_12bpp_ok ? "사용 가능 (현재 AE 설정 그대로 유지)"
                              : "사용 불가 (8bpp로 전환 + 임계값 재계산 필요)");
    return hdr_12bpp_ok;
  }

  // ------------------------------------------------------------
  // [B] 선형성 + 33us 검증
  //
  //   AE는 "밝기 = k x (레지스터 x STEP + 33us)" 를 전제로 계산한다.
  //   이게 진짜인지 확인한다.
  //
  //   방법: 조명을 고정하고 노출만 바꿔가며 찍는다.
  //         33us를 넣은 모델과 뺀 모델 중 어느 쪽이 잘 맞는지 본다.
  //         레지스터가 작을수록(짧은 노출) 두 모델 차이가 커서 구분이 쉽다.
  //
  //   ※ HDR을 끄고 단일 노출로 측정한다 (변수를 하나만 두기 위해)
  // ------------------------------------------------------------
  void run_B_linearity(DataFormat fmt = DataFormat::kRaw12bpp) {
    std::printf("\n=== [B] 선형성 + 33us 검증 ===\n");
    std::printf("조명을 고정하고 노출만 바꿔가며 찍습니다. HDR은 끕니다.\n\n");
    std::printf("%8s %10s %11s %10s %10s\n",
                "레지스터", "명령(us)", "실제(us)", "평균밝기", "포화%");

    const uint16_t sweep[] = {1u, 2u, 3u, 5u, 10u, 20u, 50u, 100u,
                              200u, 500u, 1000u, 2000u, 5000u, 10000u};

    for (uint16_t reg : sweep) {
      ShotSetup s;
      s.hdr_enabled = false;          // 단일 노출
      s.exposure1 = reg;
      s.data_format = fmt;
      s.black_col_en = true;
      s.black_col_corr = true;

      const ShotResult r = camera_.shoot(s);
      const MeasureRow& row = record("B", s, r);

      std::printf("%8u %10u %11u %10u %9u%%\n",
                  reg, row.nominal_us, row.effective_us,
                  row.mean_pixel, row.saturated_pct);

      // 포화되면 그 위로는 선형성 판정에 못 쓴다
      if (row.saturated_pct > 50u) {
        std::printf("   (포화 시작 - 이 위쪽은 선형성 판정에서 제외)\n");
        break;
      }
    }
    std::printf("\n분석 방법:\n");
    std::printf("  두 모델로 직선을 맞춰보고 잔차가 작은 쪽이 정답이다.\n");
    std::printf("    모델1 (33us 있음): 밝기 = k x (레지스터 x STEP + 33) + b\n");
    std::printf("    모델2 (33us 없음): 밝기 = k x (레지스터 x STEP)      + b\n");
    std::printf("  레지스터 1~10 구간에서 확연히 갈린다.\n");
    std::printf("  (레지스터 1이면 두 모델 예측이 명령 10us 대 실제 43us 로 갈린다)\n");
  }

  // ------------------------------------------------------------
  // [C] 블랙레벨 잔차
  //
  //   카메라에는 차광된 기준 픽셀이 있어서, 어두운 정도를 스스로 빼준다.
  //   그 보정이 충분한지, 아니면 우리가 더 빼줘야 하는지 확인한다.
  //
  //   ※ 반드시 렌즈캡을 씌우고 (또는 완전 암실에서) 측정할 것
  // ------------------------------------------------------------
  void run_C_black_level(DataFormat fmt = DataFormat::kRaw12bpp) {
    std::printf("\n=== [C] 블랙레벨 잔차 ===\n");
    std::printf("★ 렌즈캡을 씌웠는지 확인하세요!\n\n");
    std::printf("%8s %11s %12s %12s %10s\n",
                "레지스터", "실제(us)", "보정 끔", "보정 켬", "온도");

    const uint16_t sweep[] = {10u, 100u, 1000u, 10000u};

    for (uint16_t reg : sweep) {
      ShotSetup base;
      base.hdr_enabled = false;
      base.exposure1 = reg;
      base.data_format = fmt;
      base.black_col_en = true;

      base.black_col_corr = false;
      const ShotResult off = camera_.shoot(base);
      const MeasureRow& row_off = record("C_off", base, off);

      base.black_col_corr = true;
      const ShotResult on = camera_.shoot(base);
      const MeasureRow& row_on = record("C_on", base, on);

      std::printf("%8u %11u %12u %12u %9d\n",
                  reg, row_off.effective_us,
                  row_off.mean_pixel, row_on.mean_pixel, row_on.temp_c);
    }
    std::printf("\n판정:\n");
    std::printf("  '보정 켬' 값이 목표밝기의 1000분의 4(약 9픽셀)보다 훨씬 작으면\n");
    std::printf("  -> BlackLevelModel 을 계속 꺼둬도 된다.\n");
    std::printf("  그보다 크면 온도별 계수를 구해 넣어야 한다.\n");
  }

  // ------------------------------------------------------------
  // [D] 목표 밝기 확정
  //
  //   AE가 맞춰야 할 밝기를 정한다. 지금은 2240(전체의 54.7%)인데,
  //   다른 오픈소스는 16~19%를 쓴다. 약 3배 차이라 확인이 꼭 필요하다.
  //
  //   ※ 실제 운용과 비슷한 장면(밝은 곳 + 어두운 곳 공존)에서 측정할 것
  //
  //   고르는 기준 - 두 제약을 동시에 만족하는 범위의 위쪽
  //     아래 제한: 어두운 곳이 노이즈에 묻히지 않아야 한다
  //     위 제한  : 포화 존이 10~20%를 넘으면 안 된다
  //                (포화 존은 계산에서 빠지므로 너무 많으면 제어 불안정)
  // ------------------------------------------------------------
  void run_D_target_brightness(DataFormat fmt = DataFormat::kRaw12bpp) {
    std::printf("\n=== [D] 목표 밝기 확정 ===\n");
    std::printf("실제 운용과 비슷한 장면에서 측정하세요.\n\n");
    std::printf("%8s %11s %10s %8s %8s %s\n",
                "레지스터", "실제(us)", "평균밝기", "포화%", "암부%", "판정");

    const uint16_t sweep[] = {50u, 100u, 200u, 400u, 800u,
                              1600u, 3200u, 6400u, 10000u};
    const uint32_t pixel_max = format_pixel_max(fmt);

    for (uint16_t reg : sweep) {
      ShotSetup s;
      s.hdr_enabled = true;           // 실제 운용과 같은 조건
      s.exposure1 = reg;
      s.exposure2 = reg / 8u ? reg / 8u : 1u;
      s.data_format = fmt;
      s.black_col_en = true;
      s.black_col_corr = true;

      const ShotResult r = camera_.shoot(s);
      const MeasureRow& row = record("D", s, r);

      // 포화 존이 20% 이하면 목표 후보로 쓸 수 있다
      const bool usable = (row.saturated_pct <= 20u);
      std::printf("%8u %11u %10u %7u%% %7u%% %s\n",
                  reg, row.effective_us, row.mean_pixel,
                  row.saturated_pct, row.dark_pct,
                  usable ? "후보" : "포화 과다");
    }
    std::printf("\n판정: '후보' 중 가장 밝은 줄의 평균밝기를 목표로 삼는다.\n");
    std::printf("      그 값을 config.target_brightness 에 넣으면 된다.\n");
    std::printf("      (참고: 픽셀 최대값 %u)\n", pixel_max);
  }

  // ------------------------------------------------------------
  // [E] 짧은 노출 목표 확정
  //
  //   HDR에서 짧은 노출은 "밝은 부분을 살리는" 역할이다.
  //   너무 길면 밝은 곳이 하얗게 날아가고,
  //   너무 짧으면 노이즈만 남는다.
  //
  //   ※ 밝은 광원이 포함된 장면에서 측정할 것
  // ------------------------------------------------------------
  void run_E_short_target(uint16_t fixed_long = 4000u,
                           DataFormat fmt = DataFormat::kRaw12bpp) {
    std::printf("\n=== [E] 짧은 노출 목표 확정 ===\n");
    std::printf("밝은 광원이 있는 장면에서 측정하세요.\n");
    std::printf("긴 노출은 %u로 고정하고 짧은 노출만 바꿉니다.\n\n", fixed_long);
    std::printf("%8s %11s %8s %10s %s\n",
                "짧은노출", "실제(us)", "비율", "포화%", "판정");

    const uint16_t sweep[] = {1u, 2u, 5u, 10u, 25u, 50u, 100u, 200u, 400u};

    for (uint16_t reg_short : sweep) {
      if (reg_short >= fixed_long) break;

      ShotSetup s;
      s.hdr_enabled = true;
      s.exposure1 = fixed_long;
      s.exposure2 = reg_short;
      s.data_format = fmt;
      s.black_col_en = true;
      s.black_col_corr = true;

      const ShotResult r = camera_.shoot(s);
      const MeasureRow& row = record("E", s, r);

      // 짧은 노출 채널의 포화 비율을 본다
      const uint32_t short_sat = short_saturated_percent(r.zones, format_pixel_max(fmt));
      const bool safe = (short_sat <= 1u);   // 1% 이하면 안전

      std::printf("%8u %11u %7u:1 %9u%% %s\n",
                  reg_short, row.effective_us,
                  fixed_long / (reg_short ? reg_short : 1u),
                  short_sat, safe ? "안전" : "포화 발생");
    }
    std::printf("\n판정: '안전'한 것 중 가장 긴 짧은노출을 고른다.\n");
    std::printf("      짧을수록 안전하지만 노이즈가 늘기 때문이다.\n");
    std::printf("      그 실제 시간을 short_target_us_q8 에 넣는다.\n");
  }

  // ------------------------------------------------------------
  // [F] 노출 반영 지연 확인  ★짧지만 치명적★
  //
  //   "지금 지시한 노출이 이번 사진에 반영되는가,
  //    아니면 다음 사진부터 반영되는가?"
  //
  //   만약 한 장 늦게 반영된다면, AE는 이전 노출로 찍힌 사진을
  //   새 노출의 결과로 착각한다. 그러면 계산이 근본적으로 틀어진다.
  //
  //   방법: 노출을 크게 번갈아 바꾸며 찍고, 밝기가 언제 따라오는지 본다.
  // ------------------------------------------------------------
  void run_F_apply_delay(DataFormat fmt = DataFormat::kRaw12bpp) {
    std::printf("\n=== [F] 노출 반영 지연 확인 ===\n");
    std::printf("조명을 고정하고 노출을 크게 번갈아 바꿉니다.\n\n");
    std::printf("%4s %10s %12s %10s %10s\n",
                "순번", "지시한값", "헤더값", "평균밝기", "소요(ms)");

    const uint16_t pattern[] = {1000u, 100u, 1000u, 100u, 1000u, 100u};
    uint32_t index = 0;

    for (uint16_t reg : pattern) {
      ShotSetup s;
      s.hdr_enabled = false;
      s.exposure1 = reg;
      s.data_format = fmt;
      s.black_col_en = true;
      s.black_col_corr = true;
      s.sensor_rst_cfg = true;

      const ShotResult r = camera_.shoot(s);
      const MeasureRow& row = record("F", s, r);

      std::printf("%4u %10u %12u %10u %10u%s\n",
                  ++index, reg, r.header_exposure1, row.mean_pixel, r.elapsed_ms,
                  (r.header_exposure1 != reg) ? "  <- 헤더 불일치!" : "");
    }
    std::printf("\n판정:\n");
    std::printf("  밝기가 '지시한값'과 같은 줄에서 바뀌면  -> 지연 없음 (정상)\n");
    std::printf("  밝기가 한 줄 뒤에서 바뀌면              -> 1프레임 지연 (보정 필요)\n");
    std::printf("  헤더값이 지시한값과 다르면              -> 설정이 아예 안 먹은 것\n");
    std::printf("\n소요시간도 확인하세요. SENSOR_RST_CFG 때문에\n");
    std::printf("한 장당 20ms가 더 걸린다고 문서에 적혀 있습니다.\n");
    std::printf("5장이면 100ms라, 200ms 예산에서 무시 못 할 크기입니다.\n");
  }

  // ------------------------------------------------------------
  // 측정 결과를 CSV로 저장
  //   나중에 그래프를 그리거나 직선을 맞출 때 쓴다.
  // ------------------------------------------------------------
  void write_csv(std::FILE* out) const {
    std::fprintf(out,
        "item,exposure1,exposure2,nominal_us,effective_us,data_bits,hdr,"
        "black_corr,mean_pixel,saturated_pct,dark_pct,temp_c,"
        "header_exposure1,elapsed_ms,ok,fid\n");
    for (const MeasureRow& r : rows_) {
      std::fprintf(out,
          "%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%u,%u,%u,0x%04X\n",
          r.item, r.exposure1, r.exposure2, r.nominal_us, r.effective_us,
          r.data_bits, r.hdr ? 1u : 0u, r.black_corr ? 1u : 0u,
          r.mean_pixel, r.saturated_pct, r.dark_pct, r.temp_c,
          r.header_exposure1, r.elapsed_ms, r.ok ? 1u : 0u, r.fid);
    }
  }

 private:
  static uint32_t format_bits(DataFormat f) {
    switch (f) {
      case DataFormat::kRaw8bpp:  return 8u;
      case DataFormat::kRaw10bpp: return 10u;
      case DataFormat::kRaw12bpp: return 12u;
    }
    return 12u;
  }

  // 짧은 노출 채널의 포화 비율
  static uint32_t short_saturated_percent(
      const std::array<ZoneStatsHost, ZONE_COUNT_S>& zones, uint32_t pixel_max) {
    const uint32_t limit = (pixel_max * 98u) / 100u;
    uint32_t hit = 0, used = 0;
    for (const ZoneStatsHost& z : zones) {
      if (z.cnt_short == 0u) continue;
      ++used;
      if ((z.acc_short / z.cnt_short) >= limit) ++hit;
    }
    return used ? (hit * 100u) / used : 0u;
  }

  const MeasureRow& record(const char* item, const ShotSetup& s,
                            const ShotResult& r) {
    MeasureRow row;
    row.item = item;
    row.exposure1 = s.exposure1;
    row.exposure2 = s.hdr_enabled ? s.exposure2 : 0u;
    row.hdr = s.hdr_enabled;
    row.black_corr = s.black_col_corr;
    row.data_bits = format_bits(s.data_format);

    // 명령 시간 = 레지스터 x STEP,  실제 시간 = 명령 시간 + 33us
    const uint32_t step_us = (s.step == ExposureStep::kStep10us) ? 10u : 1000u;
    row.nominal_us = s.exposure1 * step_us;
    row.effective_us = row.nominal_us + 33u;

    row.ok = r.success;
    row.fid = r.fid;
    row.temp_c = r.sensor_temp_c_q8 / 256;      // Q8 -> 섭씨
    row.header_exposure1 = r.header_exposure1;
    row.elapsed_ms = r.elapsed_ms;

    if (r.success) {
      const uint32_t pixel_max = format_pixel_max(s.data_format);
      row.mean_pixel = mean_long_pixel(r.zones);
      row.saturated_pct = saturated_zone_percent(r.zones, pixel_max);
      row.dark_pct = dark_zone_percent(r.zones, pixel_max);
    }

    rows_.push_back(row);
    return rows_.back();
  }

  IEgseCamera& camera_;
  std::vector<MeasureRow> rows_;
};

}  // namespace egse
}  // namespace ae

#endif  // AE_ENABLE_EGSE_TOOLS
