// ae_hdr_ratio_schedule.hpp
//
// HDR long:short 노출 비율 결정. 정수 전용 (log() 미사용).
// AR0233의 DIRECT_LV2_T1_TO_T2 스케줄을 참고해 구현.
//
// ── 원본 테이블 분석 결과 ──────────────────────────────────
//   T1(lines)  ratio   T2 = T1/ratio
//      270       3        90.0
//      406       5        81.2
//      563       7        80.4
//      677       8        84.6
//      815       9        90.6
//     1067      11        97.0
//     1117~1302 12~14     93.1
//
//   -> T2(short)가 중간 구간 전체에서 80~97 lines로 거의 일정하다.
//      즉 이 표의 진짜 의도는 "비율 스케줄"이 아니라
//      **"short 노출을 최소값 근처에 고정하고, long만 장면에 따라 늘린다"**
//      는 전략이며, ratio는 그 결과로 따라오는 값이다.
//
// ── log() 제거 방법 ────────────────────────────────────────
//   "후보 r 중 ideal에 로그적으로 가장 가까운 것"은 log 없이 판정 가능하다.
//   단조증가 후보 r_i <= ideal <= r_{i+1} 에 대해
//     |log(ideal)-log(r_i)| < |log(r_{i+1})-log(ideal)|  <=>  ideal^2 < r_i x r_{i+1}
//   이므로 곱셈 비교만으로 동일한 결과를 얻는다. (fx::closer_in_log)
//
// ── 헌팅 방지 원칙 ─────────────────────────────────────────
//   ratio를 매 샷 바꾸면 제어 자유도가 2가 되어 secant 수렴이 무너진다.
//   따라서 **세션 시작 시 1회만 결정하고 세션 내내 고정**한다.
//   또한 ratio는 "세션 시작 노출"이 아니라 **수렴된 노출**로 재계산해
//   다음 세션에 넘긴다(콜드스타트 시작점은 장면과 무관하므로).
//
// ── HDR Merge 연계 주의 ────────────────────────────────────
//   허용 집합에 3,5,7,9,11,13 등 2의 거듭제곱이 아닌 값이 있다.
//   Merge 커널의 gain alignment는 2의 거듭제곱일 때만 Shift로 정확하므로,
//   이 스케줄을 쓰면 Q15 곱셈 경로(GLONG/GSHORT)를 사용해야 한다.

#pragma once
#include <array>
#include <cstdint>
#include "ae_fixed.hpp"

namespace ae {

// ══════════════════════════════════════════════════════════════
//  원본 표 읽는 법 — 2번째 열은 "비율"이 아니라 "T2 셔터값"이다
// ══════════════════════════════════════════════════════════════
//
//   센서 레지스터 덤프로 확인된 사실:
//     0x3012 = T1 CIT (긴 노출, 라인 수)    <- 1번째 열
//     0x3212 = T2 CIT (짧은 노출, 라인 수)  <- 2번째 열
//
//   30FPS 표를 보면 2번째 열이 8, 9, 10, ... , 30, 31 로
//   1씩 증가하는 완전한 등차수열이다.
//   비율이라면 이런 정수 수열이 나올 수 없다. 셔터값이 맞다.
//
//   onsemi 문서(AR0822 계열)도 같은 내용을 적고 있다:
//     "T2가 T1보다 작고 T3가 T2보다 작기만 하면,
//      T2와 T3를 T1과 독립적으로 설정해 어떤 노출비든 쓸 수 있다"
//
//   즉 T2는 독립적으로 정하는 노출값이고, 비율은 그 결과다.
//
// ── 두 표에서 실제 비율을 역산한 결과 ──────────────────────────
//
//   [30FPS]  T1 354~1348 (3.8배 증가)   T2 8~30 (3.8배 증가)
//            -> 비율 42.4~44.9, 평균 44
//
//   [60FPS]  T1 270~1302 (4.8배 증가)   T2 3~14 (4.7배 증가)
//            -> 비율 80.4~97.0, 평균 89
//
//   T1과 T2가 같은 배율로 함께 늘어난다.
//   즉 이 센서의 설계 의도는 "비율을 일정하게 유지"하는 것이다.
//
// ── 두 표 모두 T2가 31에서 멈추는 이유 ────────────────────────
//   onsemi 문서: "coarse integration time이 한계를 넘으면
//                 T2 적분 시간은 (상한)에 머문다"
//   31은 라인 버퍼 할당에서 오는 T2 셔터 상한이다.
//   비율 상한이 아니다. (예전에 이걸 비율로 착각해 ratio_max=31 이었다)
//
// ── MCAMv4에는 원본 표를 그대로 쓸 수 없다 ────────────────────
//
//        항목        AR0233              MCAMv4
//      ---------------------------------------------------
//        단위        라인 (14.78us)      STEP (10us)
//        T2 상한     31 (라인 버퍼 제약)  32767 (그런 제약 없음)
//        프레임      30 / 60 FPS         5 FPS
//        최대 노출   약 20 ms            104 ms
//
//   단위도 상한도 다르다. T2 셔터값을 그대로 가져오면 안 된다.
//   대신 "역산한 비율"만 가져온다.
//
//   MCSE 시험에서도 9900:1 로 촬영했고 "비율에 기술적 제한은 없다"고
//   했으므로, 31 같은 남의 집 제약을 들여올 이유가 없다.

// ══════════════════════════════════════════════════════════════
//  쓸 수 있는 비율 집합
// ══════════════════════════════════════════════════════════════
//
//   두 가지를 함께 만족하도록 만들었다.
//
//   (1) 실측 앵커 — 두 표에서 역산한 실제 비율을 그대로 넣는다
//         44 : 30FPS 실측 평균
//         89 : 60FPS 실측 평균
//        2114: 표 맨 끝 가드 항목 (0xFFFF / 31)
//
//   (2) 나머지는 √2 간격 사다리로 채운다
//        왜 √2 인가:
//          - 비율 선택은 로그 기준(closer_in_log)이라 간격도 로그로
//            균일해야 어디서든 같은 정확도가 나온다
//          - 간격이 √2면 양자화 오차가 최대 ±19% 로 제한된다
//            (2배 간격이면 ±41% 까지 벌어진다)
//          - 놀랍게도 √2 사다리는 45, 91 을 지나가는데,
//            이는 실측값 44, 89 와 거의 일치한다.
//            즉 실측 앵커와 사다리가 자연스럽게 맞물린다
//
//   범위를 1 부터 둔 이유:
//     아주 밝은 장면에서는 긴 노출 자체가 최소(43us)라
//     짧은 노출을 더 줄일 수 없다. 이때 비율 1이 되어
//     두 노출이 같아지는데, 이는 물리적 한계의 정직한 반영이다.
//     (AESessionResult::hdr_collapsed 로 상위에 알린다)
inline constexpr std::array<uint32_t, 22> kArHvModuleRatioSet =
{
    1,    2,    3,    4,    6,    8,   11,   16,   23,   32,
    44,                                    // <- 30FPS 실측 앵커
    64,
    89,                                    // <- 60FPS 실측 앵커
    128,  181,  256,  362,  512,  724, 1024, 1448,
    2114                                   // <- 원본 표 가드 항목
};

// 임계 테이블 한 줄.
// MCAMv4에서는 threshold를 반드시 "실효시간(Q8 us)"으로 넣어야 한다.
struct HdrRatioEntry
{
  uint32_t max_long_effective_us_q8;  // 이 값 이하의 long 노출이면 아래 ratio 사용
  uint32_t ratio;
};

// ── 참고용 원본 표 (기록 보존용, 계산에 직접 쓰지 말 것) ──────
//   {T1 라인수, T2 라인수}  <- 2번째 열은 비율이 아니다
//
//   MCAMv4에 쓰려면 두 가지를 모두 변환해야 한다:
//     (1) 라인 -> us  (AR0233 라인타임 약 14.78us)
//     (2) T1/T2 로 나눠 비율을 구하기
//   그래서 여기서는 기록용으로만 둔다.

// 60FPS 설정. 레지스터 덤프의 ms 주석으로 라인타임 14.78us 확인됨.
/*
inline constexpr std::array<HdrRatioEntry, 12> kAr0233Line60fpsReference =
{{
    {2,     2}, {270,   3}, {406,   5}, {563,   7},
    {677,   8}, {815,   9}, {1067, 11}, {1117, 12},
    {1210, 13}, {1302, 14}, {2000, 15}, {65535, 31},
}};
*/
// 30FPS 설정. T2가 8부터 31까지 1씩 증가한다.
//
//   ※ 원본에 {739, 18} 로 적힌 항목은 오타로 보인다.
//      앞뒤가 747(17) -> 840(19) 이고 다른 구간은 모두 46씩 증가하므로
//      747 + 46 = 793 이어야 흐름이 맞고 비율도 44.1로 이웃과 일치한다.
//      739 그대로면 T1이 오히려 감소해 단조증가가 깨지고,
//      원본 룩업 방식(if val < tbl[i][0])에서 T2=18 항목이
//      아예 선택되지 않는 죽은 코드가 된다.
//      여기서는 793으로 고쳐 기록한다.
/*
inline constexpr std::array<HdrRatioEntry, 26> kAr0233Line30fpsReference =
{{
    {2,     2}, {354,   8}, {382,   9}, {425,  10}, {470,  11},
    {516,  12}, {562,  13}, {608,  14}, {655,  15}, {701,  16},
    {747,  17}, {793,  18}, {840,  19}, {886,  20}, {932,  21},
    {978,  22}, {1024, 23}, {1071, 24}, {1117, 25}, {1163, 26},
    {1210, 27}, {1256, 28}, {1302, 29}, {1348, 30}, {2000, 31},
    {65535, 31},
}};
*/
enum class RatioMode
{
	kThresholdTable,  // (A) MCAMv4 실효시간(Q8 us) 임계 테이블 사용
	kPinnedShort,     // (B) short 목표 고정 후 ratio 역산 + 양자화 (권장)
};

struct HdrRatioParams
{
  RatioMode mode = RatioMode::kPinnedShort;

  // (B) 모드용: 고정하려는 short 노출 목표 (Q8 us).
  // 원본 센서는 T2가 3~15 라인(약 44~222us)이었다.
  // MCAMv4에서는 실측/튜닝으로 확정 필요.
  uint32_t short_target_us_q8 = 2000u * fx::kOne_q8;

  // ratio 변경 히스테리시스 (Q16).
  // 세션 간 경계에서 ratio가 왔다갔다 하는 것을 막는다.
  uint32_t change_hysteresis_q16 = fx::q16_percent(20);   // 20%

  // 쓸 수 있는 비율의 하한/상한.
  //
  //   원래 이 값이 2~31 로 되어 있었는데, 그건 원본 표의 2번째 열
  //   (실제로는 T2 셔터값)을 비율로 잘못 읽은 결과였다.
  //   원본 센서가 실제로 쓰던 비율은 80~97 이고, 끝에서는 2114 까지 간다.
  //
  //   MCSE 시험에서도 9900:1 로 촬영했고 "비율에 기술적 제한은 없다"고 했다.
  //   그래서 상한을 집합 최대치까지 열어둔다.
  //
  //   하한이 1인 이유: 아주 밝은 장면에서는 long 자체가 최소 노출이라
  //   short를 더 줄일 수 없다. 이때 1이면 long==short 가 되어
  //   사실상 HDR 없이 한 장으로 찍는 것과 같아진다.
  uint32_t ratio_min = 1;
  uint32_t ratio_max = 2114;   // 집합 최대값과 동일
};

class HdrRatioSchedule
{
 public:
  explicit HdrRatioSchedule(HdrRatioParams params = {}) : params_(params) {}

  // Threshold Table 모드를 사용할 때는 반드시 MCAMv4 실효 노출시간
  // (Q8 us)으로 변환한 테이블을 넣어야 한다. AR0233의 line 값을 그대로
  // 넣으면 단위가 달라 잘못된 ratio가 선택된다.
  void set_schedule(const HdrRatioEntry* table, uint32_t count)
  {
    table_ = table; count_ = count;
  }

  // long 노출(Q8 us)로부터 ratio 결정 (히스테리시스 미적용)
  uint32_t compute_ratio(uint32_t long_us_q8) const
  {
    // 임계 테이블이 없으면 AR0233 line 표를 임의로 적용하지 않는다.
    // 단위가 맞는 사용자 테이블이 없을 때는 Pinned Short 방식으로 안전하게 계산한다.
    const bool usable_table =
        params_.mode == RatioMode::kThresholdTable && table_ != nullptr && count_ > 0;

	const uint32_t r = usable_table
        ? lookup_table(long_us_q8)
        : derive_from_short_target(long_us_q8);

	return fx::clamp_u32(r, params_.ratio_min, params_.ratio_max);
  }

  // 세션 시작 시 호출. prev_ratio==0 이면 이전 값 없음.
  uint32_t decide_session_ratio(uint32_t long_us_q8, uint32_t prev_ratio) const
  {
    const uint32_t cand = compute_ratio(long_us_q8);

	if (prev_ratio == 0) return cand;
    // 상대 변화폭 (Q16)이 히스테리시스 이내면 이전 값 유지

	const uint32_t rel_q16 = fx::ratio_q16(fx::abs_diff(cand, prev_ratio), prev_ratio);

	return (rel_q16 < params_.change_hysteresis_q16) ? prev_ratio : cand;
  }

  // 결정된 ratio로 short 노출(Q8 us) 계산
  uint32_t short_us_q8(uint32_t long_us_q8, uint32_t ratio) const
  {
    return (ratio > 0) ? (long_us_q8 / ratio) : long_us_q8;
  }

  const HdrRatioParams& params() const { return params_; }

 private:
  // (A) 임계 테이블 룩업
  uint32_t lookup_table(uint32_t long_us_q8) const
  {
    // compute_ratio()가 table 유효성을 먼저 확인하므로 여기서는 사용자 표만 사용한다.
    const HdrRatioEntry* t = table_;
    const uint32_t n = count_;

	for (uint32_t i = 0; i < n; ++i)
	{
      if (long_us_q8 <= t[i].max_long_effective_us_q8)
	  	return t[i].ratio;
    }

    return t[n - 1].ratio;
  }

  // (B) short 목표 고정 -> ratio = long/short_target -> 허용집합 양자화
  //     단위(lines vs us)에 의존하지 않아 이식이 쉽다.
  uint32_t derive_from_short_target(uint32_t long_us_q8) const
  {
    // short 목표가 지나치게 작으면 아래 Q16 나눗셈에서 값이 넘친다.
    //   ratio_q16 = (long << 16) / target  이므로,
    //   long이 축 최대(104ms)일 때 target이 약 40us 미만이면
    //   결과가 uint32 범위를 벗어난다.
    //   물리적으로도 센서 최소 노출이 43us라 그보다 작은 목표는 의미가 없다.
    constexpr uint32_t kMinShortTargetQ8 = 64u * fx::kOne_q8;   // 64us
    const uint32_t target = (params_.short_target_us_q8 > kMinShortTargetQ8)
                                ? params_.short_target_us_q8
                                : kMinShortTargetQ8;

    // ideal 비율을 Q16으로
    const uint32_t ideal_q16 = fx::ratio_q16(long_us_q8, target);

    const auto& set = kArHvModuleRatioSet;
    const uint32_t n = static_cast<uint32_t>(set.size());

    // 범위 밖 처리
    if (ideal_q16 <= (set[0] << fx::kQ16))
		return set[0];

    if (ideal_q16 >= (set[n - 1] << fx::kQ16))
		return set[n - 1];

    // 브라케팅 후 기하평균 비교로 가까운 쪽 선택 (log 불필요)
    for (uint32_t i = 0; i + 1 < n; ++i)
	{
      const uint32_t lo = set[i], hi = set[i + 1];
      if (ideal_q16 >= (lo << fx::kQ16) && ideal_q16 <= (hi << fx::kQ16))
	  {
        return fx::closer_in_log(ideal_q16, lo, hi) ? lo : hi;
      }
    }
    return set[n - 1];
  }

  HdrRatioParams params_;
  const HdrRatioEntry* table_ = nullptr;
  uint32_t count_ = 0;
};

}  // namespace ae
