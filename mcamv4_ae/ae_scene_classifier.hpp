// ae_scene_classifier.hpp
//
// 씬 판정 + 가중 측광.
// 가중치는 상대값이므로 Q16이 필요하지 않다. 5/100/150 같은 정수 weight를
// 그대로 사용하면 최종 weighted average에서 공통 스케일이 자동으로 소거된다.


// 씬 판정 및 존 가중치 결정 + 가중 측광. 정수 전용.
//
// ── 중요 원칙 ──────────────────────────────────────────────
// 가중치는 "1번째 샷에서 1회만 판정하고 세션 내내 고정"한다.
// 매 샷 가중치를 바꾸면, secant가 관측하는 밝기 변화가
// "노출 변화 때문인지 / 가중치 변화 때문인지" 구분 불가능해져 헌팅이 재발한다.
//
// ── 측광 도메인 ────────────────────────────────────────────
// AE 피드백은 반드시 HDR Merge "이전"의 raw long 값을 쓴다.
// Adaptive Sigmoid Merge의 파라미터가 매 프레임 장면 평균에 따라 변하므로,
// Merge 출력을 쓰면 노출 변화와 Merge 곡선 변화가 뒤섞여 기울기 추정이 오염된다.


#pragma once
#include <array>
#include <cstdint>
#include "ae_fixed.hpp"
#include "ae_types.hpp"

namespace ae {

constexpr int ZONE_GRID_W_S = ZONE_GRID_W;
constexpr int ZONE_GRID_H_S = ZONE_GRID_H;
constexpr int ZONE_COUNT_S  = ZONE_COUNT;

// ae_zone_stats.cu의 AeZoneStats와 동일 레이아웃 (호스트 미러)
struct ZoneStatsHost
{
  uint64_t acc_long  = 0;
  uint64_t acc_short = 0;
  uint32_t cnt_long  = 0;						//pixels = 170u * 170u / 2u; 	//pixel Zone Size
  uint32_t cnt_short = 0;						//pixels = 170u * 170u / 2u; 	//pixel Zone Size

  uint32_t cnt_long_saturated  = 0;
  uint32_t cnt_long_dark       = 0;
  uint32_t cnt_short_saturated = 0;
  uint32_t cnt_short_dark      = 0;

  // 역할: Long 누적합을 픽셀 수로 나눠 Q8 평균을 구한다. 빈 Zone은 0.
  Brightness mean_long() const
  {
    if (cnt_long == 0) return 0;

    return static_cast<Brightness>((acc_long << fx::kQ8) / cnt_long);
  }

  // 역할: Short 누적합을 픽셀 수로 나눠 Q8 평균을 구한다. 빈 Zone은 0.
  Brightness mean_short() const
  {
    if (cnt_short == 0) return 0;

    return static_cast<Brightness>((acc_short << fx::kQ8) / cnt_short);
  }
  // 역할: Long 포화 픽셀 비율이 지정 분수 이상인지 판정한다.
  bool long_saturation_at_least(fx::Fraction threshold) const
  {
    return fx::fraction_at_least(cnt_long_saturated, cnt_long, threshold);
  }

  // 역할: Long 암부 픽셀 비율이 지정 분수 이상인지 판정한다.
  bool long_dark_at_least(fx::Fraction threshold) const
  {
    return fx::fraction_at_least(cnt_long_dark, cnt_long, threshold);
  }
};

// ============================================================
enum class SceneType
{
  kNormal,				// 일반 장면
  kBrightTop,			// 상단 강한 포화 (태양/밝은 하늘/지구 림)
  kDarkTop,				// 상단 거의 암부 (우주 배경)
  kHighContrast,		// 상/하 밝기 격차 큼
};

struct SceneClassifierParams
{
  int top_rows				= 4;			//y = 0, 1, 2,  3  -> 상단 4행
  int bottom_rows 			= 4;			//y = 8, 9, 10, 11 -> 하단 4행

  fx::Fraction bright_top_saturation = fx::percent(30);	// 상단 포화비율 임계 0.30
  fx::Fraction dark_top = fx::percent(70);				// 상단 암부비율 임계 0.70
  uint32_t high_contrast_ratio = 8;						// 상/하 밝기비 임계 8.0

  // 상대 weight. 5 : 100 : 150 = 0.05 : 1.0 : 1.5
  uint16_t suppressed_weight	 = 5;			// 0.05
  uint16_t normal_weight		 = 100;			// 1.0
  uint16_t emphasized_weight	 = 150;			// 1.5
};

struct SceneClassification
{
  SceneType type = SceneType::kNormal;				//판정된 Scene 종류
  std::array<uint16_t, ZONE_COUNT_S> weights{};		//144개 Zone에 Scene type 적용할 상대 가중치(0.05:1.0:1.5)

  // 진단/로깅용
  uint16_t top_saturation_percent = 0;				//Top Long 포화 픽셀 비율
  uint16_t top_dark_percent		  = 0;				//Top Long 암부 픽셀 비율
  Brightness top_mean			  = 0;				//Top Long 평균 밝기, Q8
  Brightness bottom_mean		  = 0;				//Bottom Long 평균 밝기, Q8
};

// ============================================================
class SceneClassifier
{
 public:
  // 역할: Scene 구역, 임계값, 상대 가중치를 저장한다.
  explicit SceneClassifier(SceneClassifierParams params = {}) : params_(params) {}

  // 역할: 첫 성공 프레임의 Long Zone으로 Scene과 고정 가중치를 결정한다.
  // 반환: Scene 종류, Zone별 가중치, 진단 통계. 빈 Zone은 제외한다.
  SceneClassification classify(
      const std::array<ZoneStatsHost, ZONE_COUNT_S>& zones) const
  {
    SceneClassification result;
    const int top_rows		 = clamp_row_count(params_.top_rows);
    const int bottom_rows	 = clamp_row_count(params_.bottom_rows);

    uint64_t top_long_sat 		 = 0;			//상단의 Long 포화 픽셀 수
    uint64_t top_long_dark 		 = 0;			//상단의 Long 암부 픽셀 수
    uint64_t top_long_count 	 = 0;			//상단의 전체 Long 픽셀 수
    uint64_t top_mean_sum 		 = 0;			//상단 Zone들의 Long 평균 합
    uint64_t bottom_mean_sum	 = 0;			//하단 Zone들의 Long 평균 합
    uint32_t top_zone_count		 = 0;			//Long 픽셀이 있는 상단 Zone 수
    uint32_t bottom_zone_count	 = 0;			//Long 픽셀이 있는 하단 Zone 수

    for (int y = 0; y < ZONE_GRID_H_S; ++y)
	{
      for (int x = 0; x < ZONE_GRID_W_S; ++x)
	  {
        const ZoneStatsHost& zone = zones[y * ZONE_GRID_W_S + x];

        if (y < top_rows && zone.cnt_long > 0u)
		{//y = 0, 1, 2, 3
          top_long_sat		 += zone.cnt_long_saturated;
          top_long_dark 	 += zone.cnt_long_dark;
          top_long_count 	 += zone.cnt_long;
          top_mean_sum		 += zone.mean_long();
          ++top_zone_count;
        }
		else if (y >= ZONE_GRID_H_S - bottom_rows && zone.cnt_long > 0u)
		{//y = 8, 9, 10, 11
          bottom_mean_sum	+= zone.mean_long();
          ++bottom_zone_count;
        }
      }
    }

	//상단 평균 계산
    result.top_mean = top_zone_count
        ? static_cast<Brightness>(top_mean_sum / top_zone_count)
        : 0;

	//하단 평균 계산
    result.bottom_mean = bottom_zone_count
        ? static_cast<Brightness>(bottom_mean_sum / bottom_zone_count)
        : 0;

    if (top_long_count > 0)
	{
	  //상단 포화율 계산
      result.top_saturation_percent = static_cast<uint16_t>(
          (top_long_sat * 100u) / top_long_count);

	  //상단 암부율 계산
      result.top_dark_percent = static_cast<uint16_t>(
          (top_long_dark * 100u) / top_long_count);
    }

	//BrightTop 조건 계산 :: 기본 임계값은 30%
    const bool bright_top = fx::fraction_at_least(
        top_long_sat, top_long_count, params_.bright_top_saturation);		//top_long_sat×100 ≥ top_long_count×30
	//DarkTop 조건 계산 :: 기본 임계값은 70%
    const bool dark_top = fx::fraction_at_least(
        top_long_dark, top_long_count, params_.dark_top);					//top_long_dark×100 ≥ top_long_count×70

    // 상/하 대비 = 큰쪽/작은쪽 (Q16)
    bool high_contrast = false;
    if (result.top_mean > 0 && result.bottom_mean > 0)
	{
	  //상·하단 중 큰 밝기 선택
      const Brightness high = (result.top_mean > result.bottom_mean)
          ? result.top_mean : result.bottom_mean;
	  //상·하단 중 작은 밝기 선택
      const Brightness low = (result.top_mean > result.bottom_mean)
          ? result.bottom_mean : result.top_mean;

	  //두 영역의 밝기 차이 x8
	  high_contrast = static_cast<uint64_t>(high) >=		//High ≥ Low x8
                      static_cast<uint64_t>(low) * params_.high_contrast_ratio;
    }

    if (bright_top)
	{	//상단 포화율이 30% 이상이면
      result.type = SceneType::kBrightTop;
    }
	else if (dark_top)
	{	//상단 암부율이 70% 이상이면
      result.type = SceneType::kDarkTop;
    }
	else if (high_contrast)
	{	//상단과 하단 밝기 차이가 8배 이상이면
      result.type = SceneType::kHighContrast;
    }
	else
	{
      result.type = SceneType::kNormal;
    }

    result.weights = build_weights(result.type);
    return result;
  }

 private:
  // 역할: 사용자 행 수를 0~12 범위로 제한한다.
  static int clamp_row_count(int rows)
  {
    if (rows < 0) return 0;
    return (rows > ZONE_GRID_H_S) ? ZONE_GRID_H_S : rows;
  }

  // 역할: Scene 종류를 144개 정수 상대 가중치로 변환한다.
  std::array<uint16_t, ZONE_COUNT_S> build_weights(SceneType type) const
  {
    std::array<uint16_t, ZONE_COUNT_S> weights{};
    const int top_rows		 = clamp_row_count(params_.top_rows);
    const int bottom_rows 	 = clamp_row_count(params_.bottom_rows);

    for (int y = 0; y < ZONE_GRID_H_S; ++y)
	{
      for (int x = 0; x < ZONE_GRID_W_S; ++x)
	  {
        const int id = y * ZONE_GRID_W_S + x;
        const bool top = y < top_rows;							//y = 0, 1, 2, 3      => 상단 4행
        const bool bottom = y >= ZONE_GRID_H_S - bottom_rows;	//y = 8, 9, 10, 11	  => 하단 4행
        const bool middle = !top && !bottom;					//y = 4, 5, 6, 7      => 중앙 4행

        switch (type)
		{
          case SceneType::kBrightTop:
            weights[id] = top ? params_.suppressed_weight				// top    :   5%
                              : (bottom ? params_.emphasized_weight		// bottom : 150%
                                        : params_.normal_weight);		// middle : 100%
            break;
          case SceneType::kDarkTop:
            weights[id] = top ? params_.suppressed_weight				// Top : 5%
                              : params_.normal_weight;					// bottom/middle 100%
            break;
          case SceneType::kHighContrast:
            weights[id] = middle ? params_.emphasized_weight			// bottom/middle : 150%
                                 : params_.suppressed_weight;			// Top : 5%
            break;
          case SceneType::kNormal:
          default:
            weights[id] = params_.normal_weight;						// All : 100%
            break;
        }
      }
    }
    return weights;
  }

  SceneClassifierParams params_;
};

// ============================================================
// 존별 통계 + 확정 가중치 -> AE 제어용 단일 밝기 스칼라 (Q8)
//
// 클리핑 존은 노출을 바꿔도 값이 변하지 않아 기울기 추정을 왜곡하므로 제외한다.
// ============================================================
struct MeteringResult
{
  Brightness weighted_mean = 0;			//유효 Long Zone의 가중 평균 밝기, Q8
  int valid_zone_count = 0;				//실제 평균 계산에 포함한 Zone 수
  bool trust_linear_model = true;		//Inversion·Secant 계산 신뢰도
};

// 역할: Long Zone(144)의 포화/암부를 제거하고 Scene 가중 평균 밝기를 계산한다.
// 반환: Q8 평균, 유효 Zone 수, 선형 모델 신뢰 가능 여부.
inline MeteringResult compute_weighted_metering(
    const std::array<ZoneStatsHost, ZONE_COUNT_S>& zones,
    const std::array<uint16_t, ZONE_COUNT_S>& weights,
    fx::Fraction saturation_reject = fx::percent(50),		// 0.5
    fx::Fraction dark_reject = fx::percent(90),				// 0.9
    int min_valid_zones = 30)
{
  MeteringResult result;
  uint64_t weighted_sum = 0;  // sum(Q8 평균 x 정수 상대 가중치)
  uint64_t total_weight = 0;  // 정수 상대 가중치 합

  for (int i = 0; i < ZONE_COUNT_S; ++i)	//(0~143)
  {
    const ZoneStatsHost& zone = zones[i];

	if (zone.cnt_long == 0u)
		continue;

    if (zone.long_saturation_at_least(saturation_reject))	//(Long포화픽셀수​/Long전체픽셀수)​>50%
		continue;

    if (zone.long_dark_at_least(dark_reject))				//(Long암부픽셀수/Long전체픽셀수)​>90%
		continue;

    const uint16_t weight = weights[i];

    if (weight == 0)
		continue;

	//각 Zone 평균에 가중치를 곱한 값의 합
    weighted_sum += static_cast<uint64_t>(zone.mean_long()) * weight;
	//평균에 포함된 Zone 가중치의 합
    total_weight += weight;
    ++result.valid_zone_count;
  }

  //30 more active zones
  result.trust_linear_model = result.valid_zone_count >= min_valid_zones;  // X > 30

  if (total_weight > 0)
  {
    result.weighted_mean = static_cast<Brightness>(weighted_sum / total_weight);
  }
  else
  {
    // 전 유효 존이 클리핑된 경우 비어 있지 않은 Long Zone의 단순 평균.
    uint64_t sum = 0;
    uint32_t populated_zone_count = 0;
    for (const ZoneStatsHost& zone : zones)
	{
      if (zone.cnt_long == 0u)
	  	continue;

      sum += zone.mean_long();
      ++populated_zone_count;
    }

    result.weighted_mean = (populated_zone_count > 0u)
        ? static_cast<Brightness>(sum / populated_zone_count)
        : 0u;

    result.trust_linear_model = false;
  }

  return result;
}

}  // namespace ae
