// ae_log_exposure_axis.hpp
//
//    "노출 눈금자"를 만든다.
//    눈금 1칸을 올리면 밝기가 항상 약 1%씩 늘어난다.
//
//         눈금(코드)  ◀──────▶  실제 노출시간  ◀──────▶  레지스터
//          108~994              us 단위             카메라에
//        (AE가 다루는 값)                          써넣는 값
//
//
// ══════════════════════════════════════════════════════════════
//  핵심 발상:  "매번 계산" 하지 않고  "시작할 때 표를 펼쳐둔다"
// ══════════════════════════════════════════════════════════════
//
//   눈금은 108~994, 딱 887칸뿐이다.
//   그러니 매번 복잡한 변환을 하느니, 프로그램 시작할 때
//   887칸 전부를 미리 계산해서 배열에 적어두면 된다.
//
//   [시작할 때 딱 한 번]
//       887칸을 돌면서 표를 채운다
//         눈금 → 시간, 눈금 → 레지스터, 눈금 → 1칸 변화율
//
//   [그 다음부터는 계속]
//       눈금 → 시간     : 배열에서 꺼내기        (계산 없음)
//       눈금 → 레지스터 : 배열에서 꺼내기        (계산 없음)
//       시간 → 눈금     : 배열에서 이분탐색       (나눗셈 없음)
//
//   그래서 실제 운용 중에는 나눗셈도, 64비트 곱셈도, 표 보간도,
//   오차 보정도 전혀 일어나지 않는다. 배열 읽기와 비교뿐이다.
//
//   좋은 점
//     1. 이해하기 쉽다   - "표 만들고 찾아보기" 하나면 끝
//     2. 빠르다          - 런타임 나눗셈/보간 0회
//     3. 시간이 일정하다 - 입력값에 따라 걸리는 시간이 안 변한다
//                          (우주/임베디드에서 중요한 성질)
//     4. 오차 보정 불필요 - 표에 이미 정답이 들어있다
//
//   드는 비용
//     메모리 약 8.7KB (887칸 x 10바이트). 호스트에서는 무시할 수준.
//
// ══════════════════════════════════════════════════════════════
//
// ── 왜 눈금(로그 코드)이라는 걸 따로 두는가 ────────────────────
//   카메라 레지스터는 그냥 정수(0~32767)라서 그대로 쓰면 이렇게 된다.
//     짧은 노출: 1칸(10us) 올리면 +10%   -> 너무 거칠다
//     긴 노출  : 1칸(10us) 올리면 +0.1%  -> 노이즈 흔들림이 그대로 전달
//
//   그래서 "1칸 = 밝기 약 1%"가 되도록 만들어둔 로그 곡선(gnAeShtTbl)을
//   AE의 계산 축으로 쓰고, 카메라에 쓸 때만 레지스터로 바꾼다.
//
//   덤: 긴 노출에서는 눈금 1칸이 레지스터 수십~수백 칸을 건너뛴다.
//       그래서 미세한 레지스터 흔들림이 저절로 걸러진다.
//
// ── 시간이 두 종류인 이유 ──────────────────────────────────────
//   카메라 설명서: 실제 노출 = 레지스터값 x STEP + 33us
//
//     명령 시간 : 레지스터값 x STEP        <- 표를 만들 때만 쓰는 중간값
//     실제 시간 : 명령 시간 + 33us          <- AE 계산은 전부 이것
//
//   밝기는 실제 시간에 비례한다. 33us를 빼먹으면 짧은 노출에서 크게 틀어진다
//   (레지스터 1이면 10us 대 43us로 4배 차이).
//
//   이 파일에서 "명령 시간"은 표를 만드는 생성자 안에서만 등장한다.
//   표가 다 만들어진 뒤에는 신경 쓸 필요가 없다.
//
// ── 5fps에서 노출 상한이 104ms인 이유 ──────────────────────────
//   한 프레임 200ms 중 고정으로 나가는 시간:
//     리셋 20ms + FOT 0.139ms + 읽기 63.41ms + ROI전송 12.3ms = 95.849ms
//   200 - 95.849 = 104.151ms  ->  노출에 쓸 수 있는 시간

#pragma once
#include <cstdint>
#include "ae_fixed.hpp"
#include "ae_shutter_lut.hpp"
#include "ae_types.hpp"

namespace ae {

// 표로 만들 수 있는 눈금 개수의 상한.
// 배열을 고정 크기로 잡아 힙 할당을 피한다(우주용 SW에서 선호되는 방식).
constexpr uint32_t kMaxAxisCodes = 1024u;

struct LogExposureAxisParams
{
  // 쓸 수 있는 눈금 범위.
  //   code_min은 "레지스터가 1 이상 되는 최소 눈금"이어야 한다.
  //   그보다 낮으면 레지스터가 0이 되어 노출을 표현할 수 없다.
  uint32_t code_min = 108;					//{98,168}~{112,211} => 168+((211−168)(108−98)​/(112-97))= *198
  uint32_t code_max = 994;					//{994, * 4089531}

  // 맨 위 눈금(code_max)이 가리키는 명령 시간 (Q8 us).
  // 5fps에서 노출에 쓸 수 있는 104ms 기준.
  ExposureUs max_nominal_time = 104000u * fx::kOne_q8;		//104000 × 256	= 26,624,000

  ExposureStep step = ExposureStep::kStep10us;
};

class LogExposureAxis
{
 public:
  // 생성자에서 표를 전부 채운다. 여기서만 계산을 하고, 이후로는 찾아보기만 한다.
  LogExposureAxis(const ShutterLUT& lut, LogExposureAxisParams params = {})
      : params_(params)
  {
    build_tables(lut);
  }

  // ==============================================================
  // 눈금 -> 무엇  (전부 배열에서 꺼내기만 한다)
  // ==============================================================

  // 눈금 -> 실제 노출시간.  AE의 모든 계산이 이 값을 쓴다.
  //T-effective​=Register x STEP + 33μs
  ExposureUs code_to_effective_time(uint32_t code) const
  {
    return effective_time_[index_of(code)];
  }

  // 눈금 -> 카메라 레지스터값.
  uint16_t code_to_register(uint32_t code) const
  {
    return register_[index_of(code)];
  }

  // 눈금 1칸을 올리면 밝기가 몇 % 변하는가 (Q16).
  //   Guard가 "이 구간에서 허용할 오차"를 정할 때 쓴다.
  // ※ 눈금 범위(code_min~code_max) 밖이면 0을 돌려준다.
  //  LogStepRatio = ((T-nominal(code+1)−Tnominal(code)) << kQ16) / T-nominal(code)
  Ratio nominal_step_ratio(uint32_t code) const
  {
    if (code < params_.code_min) return 0u;

	return step_ratio_[index_of(code)];
  }

  // 두 눈금이 결국 같은 레지스터가 되는가?
  //   같다면 카메라는 미동도 하지 않는다. 다시 찍어봐야 결과가 같으므로
  //   AE는 "조정할 의미 없음"으로 보고 촬영을 생략한다.
  bool is_same_register(uint32_t a, uint32_t b) const
  {
    return register_[index_of(a)] == register_[index_of(b)];
  }

  // ==============================================================
  // 시간 -> 무엇
  // ==============================================================

  // 실제 노출시간 -> 가장 가까운 눈금.
  //   표가 이미 오름차순이므로 이분탐색으로 바로 찾는다.
  //   (예전처럼 역방향 보간을 하고 내림 오차를 보정할 필요가 없다)
  uint32_t effective_time_to_code(ExposureUs effective_time) const
  {
    return params_.code_min + find_closest_index(effective_time);
  }

  // 실제 노출시간 -> 레지스터값.
  //   33us를 빼고 STEP으로 반올림한다. (표를 거치지 않는 직접 계산)
  uint16_t effective_time_to_register(ExposureUs effective_time) const
  {
    if (effective_time <= kExposureOverhead) return 1u;

	const ExposureUs command_time = effective_time - kExposureOverhead;

    const uint64_t reg = fx::udiv_round(command_time, step_size(params_.step));

    const uint32_t clamped =
        fx::clamp_u32(static_cast<uint32_t>(reg), EXPOSURE_MIN, EXPOSURE_MAX);

    return static_cast<uint16_t>(clamped < 1u ? 1u : clamped);
  }

  // 레지스터값 -> 실제 노출시간.   (레지스터 x STEP + 33us)
  ExposureUs register_to_effective_time(uint16_t reg) const
  {
    return static_cast<ExposureUs>(reg) * step_size(params_.step) + kExposureOverhead;
  }

  // ==============================================================
  // 설정값 읽기
  // ==============================================================

  uint32_t code_min() const { return params_.code_min; }		//uint32_t code_min = 108;
  uint32_t code_max() const { return params_.code_max; }	 	// uint32_t code_max = 994;
  ExposureStep step() const { return params_.step; }
  ExposureUs step_time() const { return step_size(params_.step); }
  ExposureUs max_nominal_time() const { return params_.max_nominal_time; }

 private:
  // ==============================================================
  // 표 만들기 — 시작할 때 딱 한 번만 실행된다.
  //   여기서만 LUT 보간과 나눗셈을 쓴다.
  // ==============================================================
  void build_tables(const ShutterLUT& lut)
  {
    // 눈금 범위가 배열을 넘지 않도록 잘라둔다
    if (params_.code_max < params_.code_min)
    {
		params_.code_max = params_.code_min;
    }

    if (params_.code_max - params_.code_min >= kMaxAxisCodes)
	{
      params_.code_max = params_.code_min + kMaxAxisCodes - 1u;
    }

    count_ = params_.code_max - params_.code_min + 1u;

    uint32_t max_weight = lut.code_to_q7(params_.code_max);		//{994, *4,089,531}

    if (max_weight == 0u)
		max_weight = 1u;   // 0으로 나누는 것 방지

    // 각 눈금마다: 명령시간 -> 레지스터 -> 실제시간 순으로 채운다
    for (uint32_t i = 0; i < count_; ++i)
	{
      const uint32_t code = params_.code_min + i;			//108 + 0, 1, 2, 3, ...
															//{98,168}~{112,211}
      const ExposureUs command_time = weight_to_command_time(
          lut.code_to_q7(code), max_weight);

		// => 168+((211−168)(108−98)​/(112-97))=198 ::lut.code_to_q7(108)
		//  min_nominal_time=   (198×26,624,000)/4,089,531 = 1,289

      nominal_time_[i] = command_time;

      register_[i] = command_time_to_register(command_time);

      effective_time_[i] = register_to_effective_time(register_[i]);
    }

    // 1칸 변화율은 이웃한 두 명령시간의 차이로 구한다.
    //   실제시간이 아니라 명령시간으로 재는 이유:
    //   실제시간은 레지스터에 맞춰 뭉개져 있어서, 이웃끼리 같은 값이 되면
    //   변화율이 0으로 나와버린다. 명령시간은 뭉개지지 않은 연속값이라
    //   "로그 축 본래의 변화율"을 제대로 반영한다.
    for (uint32_t i = 0; i < count_; ++i)
	{
      const ExposureUs here = nominal_time_[i];

      if (here == 0u)
	  {
	  	step_ratio_[i] = 0u;
		continue;
	  }

      // 맨 끝 눈금은 다음 칸이 없으므로 변화율 0
      const ExposureUs next = (i + 1u < count_) ? nominal_time_[i + 1u] : here;

      step_ratio_[i] = fx::ratio_q16(fx::abs_diff(next, here), here);
    }
  }

  // LUT 가중치 -> 명령시간 (Q8 us).  비례식 한 번.
  //   "이 눈금의 가중치가 최대 가중치의 몇 분의 몇인가"를 최대 시간에 곱한다.
  //   중간값이 매우 커서(최대 약 1.1e14) 64비트로 계산한다.
  ExposureUs weight_to_command_time(uint32_t weight, uint32_t max_weight) const
  {
    return static_cast<ExposureUs>(
        (static_cast<uint64_t>(weight) * params_.max_nominal_time) / max_weight);
  }

  // 명령시간 -> 레지스터값.  STEP 단위로 반올림.
  uint16_t command_time_to_register(ExposureUs command_time) const
  {
    const uint64_t reg = fx::udiv_round(command_time, step_size(params_.step));

    return static_cast<uint16_t>(
        fx::clamp_u32(static_cast<uint32_t>(reg), EXPOSURE_MIN, EXPOSURE_MAX));
  }

  // ==============================================================
  // 표 찾아보기
  // ==============================================================

  // 눈금 -> 배열 위치.  범위를 벗어나면 양 끝으로 잘라낸다.
  uint32_t index_of(uint32_t code) const
  {
    if (code <= params_.code_min) return 0u;
    if (code >= params_.code_max) return count_ - 1u;

    return code - params_.code_min;
  }

  // 목표 시간에 가장 가까운 배열 위치를 이분탐색으로 찾는다.
  //
  //   표는 오름차순이므로, 먼저 "목표보다 작지 않은 첫 위치"를 찾은 뒤
  //   그 자리와 바로 앞자리 중 더 가까운 쪽을 고르면 된다.
  //
  //   ※ 명령시간 기준으로 찾는다. 실제시간은 레지스터에 맞춰 뭉개져 있어
  //      같은 값이 여럿 나오는데, 명령시간은 눈금마다 값이 달라
  //      "어느 눈금이 목표에 가장 가까운가"를 정확히 가릴 수 있다.
  uint32_t find_closest_index(ExposureUs effective_time) const
  {
    // 실제시간에서 33us를 빼 명령시간으로 맞춘 뒤 찾는다
    const ExposureUs target = (effective_time > kExposureOverhead)
                                  ? (effective_time - kExposureOverhead)
                                  : 0u;

    if (target <= nominal_time_[0])
		return 0u;

    if (target >= nominal_time_[count_ - 1u])
		return count_ - 1u;

    uint32_t low = 0u;
    uint32_t high = count_ - 1u;

    while (low < high)
	{
      const uint32_t mid = low + (high - low) / 2u;

      if (nominal_time_[mid] < target)
	  	low = mid + 1u;
      else high = mid;
    }

    // low = 목표보다 작지 않은 첫 위치. 바로 앞자리와 비교해 가까운 쪽 선택.
    if (low == 0u)
		return 0u;

    const ExposureUs gap_here = fx::abs_diff(nominal_time_[low], target);
    const ExposureUs gap_prev = fx::abs_diff(nominal_time_[low - 1u], target);
    return (gap_prev <= gap_here) ? (low - 1u) : low;
  }

  LogExposureAxisParams params_;
  uint32_t count_ = 0u;                          // 실제로 채운 눈금 개수

  // 눈금별 미리 계산해둔 표 (배열 위치 = 눈금 - code_min)
  ExposureUs nominal_time_[kMaxAxisCodes]   = {};  // 명령시간 (되찾기용)		//=INT(interpolated_q7*max_nominal_time_Q8/max_q7)
  ExposureUs effective_time_[kMaxAxisCodes] = {};  // 실제 노출시간			//=register*step_Q8+overhead_Q8
  uint16_t   register_[kMaxAxisCodes]       = {};  // 카메라 레지스터값			//=MIN(32767,MAX(0,INT((nominal_Q8+step_Q8/2)/step_Q8))) //32767=(2^15)-1
  Ratio      step_ratio_[kMaxAxisCodes]     = {};  // 1칸 변화율 (Q16)		//=INT(ABS(next_nominal_Q8-current_nominal_Q8)*65536/current_nominal_Q8) //1u<<kQ16::65536
};

}  // namespace ae
