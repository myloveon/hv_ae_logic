// ae_zone_stats.cu
//
// 12x12 존별 통계 reduction 커널.
//
// 설계 배경:
//   - AE의 제어 피드백은 "Merge 이전의 raw 값"을 써야 한다.
//     Merge(Adaptive Sigmoid)의 파라미터(c, target_long/short, sigma)는 매 프레임
//     장면 평균에 따라 변하므로, Merge 출력을 피드백으로 쓰면 "노출 변화"와
//     "Merge 곡선 변화"가 섞여 secant의 기울기 추정이 오염된다.
//   - 여기서 구한 존별 통계는 AE와 Merge가 공유한다.
//     (전체 평균 acc_long_sum/acc_short_sum은 존별 합을 모두 더하면 나오므로,
//      기존 compute_frame_params()를 그대로 재사용할 수 있다.)
//
// 존 분할 방식 (확정):
//   2048 % 12 != 0 이므로, 중앙 2040x2040만 사용하고 가장자리 4픽셀씩 버린다.
//   -> 존 크기는 균일하게 170x170. 가장자리는 렌즈 비네팅/왜곡이 심한 영역이라
//      측광에서 제외하는 편이 오히려 정확하다.
//
// 인터리브 가정: 짝수 행(y%2==0) = Long, 홀수 행(y%2==1) = Short

#include <cstdint>
#include <cuda_runtime.h>

#define AE_ZONE_GRID_W   12
#define AE_ZONE_GRID_H   12
#define AE_ZONE_COUNT    (AE_ZONE_GRID_W * AE_ZONE_GRID_H)   // 144

// 중앙 크롭 설정 (2048 -> 2040, 좌우상하 4픽셀씩 제외)
#define AE_CROP_OFFSET   4
#define AE_CROP_SIZE     2040
#define AE_ZONE_SIZE     (AE_CROP_SIZE / AE_ZONE_GRID_W)      // 170

// 존별 통계 (호스트/디바이스 공용 구조)
struct AeZoneStats
{
  uint64_t acc_long;      // 짝수 행 픽셀 합
  uint64_t acc_short;     // 홀수 행 픽셀 합
  uint32_t cnt_long;      // 짝수 행 픽셀 개수 (정규화용)
  uint32_t cnt_short;     // 홀수 행 픽셀 개수
  uint32_t cnt_long_saturated;	// long 채널 포화 픽셀 수 -> "하늘/태양" 씬 판정용
  uint32_t cnt_long_dark;	// short 채널 암부 클리핑 픽셀 수 -> "그림자/우주" 판정용
  uint32_t cnt_short_saturated;
  uint32_t cnt_short_dark;
};

// ============================================================
// 존별 통계 수집 커널
// 역할: 중앙 2040x2040 RAW를 12x12 Zone으로 나누고 Long/Short 통계를 계산한다.
// 입력 조건: raw는 최소 2044x2044 접근 가능, block=(16,16), grid=(12,12).
//   - 블록 1개가 존 1개를 담당 (gridDim = 12 x 12)
//   - 블록 내에서 shared memory reduction 후 존별 결과 1회 기록
//     (atomicAdd를 글로벌 메모리에 남발하지 않기 위함)
// ============================================================
__global__ void ae_zone_stats_kernel(
    const uint16_t* __restrict__ raw,   // [H, W] 16bit linear, 인터리브 상태
    AeZoneStats* __restrict__ out,      // [144]
    int W,
    uint16_t sat_threshold,             // Long/Short 포화 판정 임계값
    uint16_t dark_threshold)
{          // Long/Short 암부 판정 임계값

  const int zone_x = blockIdx.x;
  const int zone_y = blockIdx.y;
  const int zone_id = zone_y * AE_ZONE_GRID_W + zone_x;

  // 이 존이 담당하는 원본 이미지 영역 (중앙 크롭 오프셋 반영)
  const int x0 = AE_CROP_OFFSET + zone_x * AE_ZONE_SIZE;
  const int y0 = AE_CROP_OFFSET + zone_y * AE_ZONE_SIZE;

  // 스레드별 부분합 (블록당 256 스레드 가정)
  __shared__ uint64_t s_acc_long[256];
  __shared__ uint64_t s_acc_short[256];
  __shared__ uint32_t s_cnt_long[256];
  __shared__ uint32_t s_cnt_short[256];
  __shared__ uint32_t s_long_sat[256];
  __shared__ uint32_t s_long_dark[256];
  __shared__ uint32_t s_short_sat[256];
  __shared__ uint32_t s_short_dark[256];

  const int tid = threadIdx.y * blockDim.x + threadIdx.x;
  const int nthreads = blockDim.x * blockDim.y;

  uint64_t acc_l = 0, acc_s = 0;
  uint32_t cnt_l = 0, cnt_s = 0;
  uint32_t long_sat = 0, long_dark = 0;
  uint32_t short_sat = 0, short_dark = 0;

  // 존 내부(170x170)를 스레드들이 grid-stride로 나눠 처리
  const int total_pixels = AE_ZONE_SIZE * AE_ZONE_SIZE;

  for (int idx = tid; idx < total_pixels; idx += nthreads)
  {
    const int ly = idx / AE_ZONE_SIZE;
    const int lx = idx - ly * AE_ZONE_SIZE;
    const int gy = y0 + ly;
    const int gx = x0 + lx;

    const uint16_t v = raw[gy * W + gx];

    if ((gy & 1) == 0)
	{
      // Long 채널 (짝수 행)
      acc_l += v;
      ++cnt_l;
      if (v >= sat_threshold) ++long_sat;
      if (v <= dark_threshold) ++long_dark;
    }
	else
	{
      // Short 채널 (홀수 행)
      acc_s += v;
      ++cnt_s;
      if (v >= sat_threshold) ++short_sat;
      if (v <= dark_threshold) ++short_dark;
    }
  }

  s_acc_long[tid]  = acc_l;
  s_acc_short[tid] = acc_s;
  s_cnt_long[tid]  = cnt_l;
  s_cnt_short[tid] = cnt_s;
  s_long_sat[tid]   = long_sat;
  s_long_dark[tid]  = long_dark;
  s_short_sat[tid]  = short_sat;
  s_short_dark[tid] = short_dark;
  __syncthreads();

  // 블록 내 tree reduction
  for (int stride = nthreads / 2; stride > 0; stride >>= 1)
  {
    if (tid < stride)
	{
      s_acc_long[tid]  += s_acc_long[tid + stride];
      s_acc_short[tid] += s_acc_short[tid + stride];
      s_cnt_long[tid]  += s_cnt_long[tid + stride];
      s_cnt_short[tid] += s_cnt_short[tid + stride];
      s_long_sat[tid]   += s_long_sat[tid + stride];
      s_long_dark[tid]  += s_long_dark[tid + stride];
      s_short_sat[tid]  += s_short_sat[tid + stride];
      s_short_dark[tid] += s_short_dark[tid + stride];
    }
    __syncthreads();
  }

  if (tid == 0)
  {
    out[zone_id].acc_long      = s_acc_long[0];
    out[zone_id].acc_short     = s_acc_short[0];
    out[zone_id].cnt_long      = s_cnt_long[0];
    out[zone_id].cnt_short     = s_cnt_short[0];
    out[zone_id].cnt_long_saturated  = s_long_sat[0];
    out[zone_id].cnt_long_dark       = s_long_dark[0];
    out[zone_id].cnt_short_saturated = s_short_sat[0];
    out[zone_id].cnt_short_dark      = s_short_dark[0];
  }
}

// ============================================================
// 호스트 측 런처
// ============================================================
// 역할: 고정 grid/block으로 Zone 통계 커널을 실행한다.
// 주의: width/height 최소 2044와 CUDA 오류 확인은 호출자가 보장한다.
inline void launch_ae_zone_stats(const uint16_t* d_raw, AeZoneStats* d_out,
                                  int W, uint16_t sat_threshold,
                                  uint16_t dark_threshold, cudaStream_t stream)
{
  dim3 grid(AE_ZONE_GRID_W, AE_ZONE_GRID_H);
  dim3 block(16, 16);  // 256 threads
  ae_zone_stats_kernel<<<grid, block, 0, stream>>>(d_raw, d_out, W,
                                                    sat_threshold, dark_threshold);
}
