#!/usr/bin/env python3
"""
egse_raw.py — 저장해둔 RAW 파일을 읽어 12x12 존 통계로 바꾸는 모듈

┌──────────────────────────────────────────────────────────────┐
│ 이 파일이 하는 일                                              │
│                                                              │
│   EGSE 프로그램으로 찍어서 저장한 RAW 파일을 읽고,              │
│   AE가 쓰는 12x12 칸 통계로 바꿔준다.                          │
└──────────────────────────────────────────────────────────────┘

왜 Python으로 따로 하는가
  카메라를 직접 제어하는 쪽(C++ egse_tool)과 달리,
  이건 이미 저장된 파일을 PC에서 분석하는 도구다.
  실시간도 아니고 카메라에 올라가지도 않으므로
  계산이 편한 Python으로 하는 편이 낫다.
  (정수 전용 규칙은 카메라에 올라가는 C++ 코드에만 적용된다)

필요한 것
  pip install numpy
"""

import os
import numpy as np

# ==============================================================
# 규격 (C++ ae_zone_stats.cu 와 같은 값이어야 한다)
# ==============================================================
RAW_WIDTH = 2048
RAW_HEIGHT = 2048

# 2048을 12로 나누면 딱 떨어지지 않는다(170.67).
# 그래서 가장자리 4픽셀씩 빼고 가운데 2040만 쓴다. 2040/12 = 170 으로 딱 떨어진다.
# (가장자리는 렌즈 비네팅이 심해서 측광에서 빼는 편이 오히려 정확하다)
CROP_OFFSET = 4
CROP_SIZE = 2040
ZONE_GRID = 12
ZONE_SIDE = CROP_SIZE // ZONE_GRID      # 170
ZONE_COUNT = ZONE_GRID * ZONE_GRID      # 144

# 다운로드 포맷별 픽셀 최대값 (ICD Table 52)
PIXEL_MAX = {8: 255, 10: 1023, 12: 4095}


# ==============================================================
# RAW 파일 크기 계산
# ==============================================================
def expected_file_size(bits, width=RAW_WIDTH, height=RAW_HEIGHT):
    """이 포맷이면 파일이 몇 바이트여야 하는지"""
    pixels = width * height
    if bits == 8:
        return pixels                    # 픽셀 1개 = 1바이트
    if bits == 10:
        return pixels // 4 * 5           # 픽셀 4개 = 5바이트
    return pixels // 2 * 3               # 12bpp: 픽셀 2개 = 3바이트


# ==============================================================
# 눌러 담긴 바이트를 픽셀값으로 풀어내기
# ==============================================================
def unpack(data, bits, width=RAW_WIDTH, height=RAW_HEIGHT):
    """
    RAW 바이트 -> 2차원 픽셀 배열

    8bpp  : 픽셀 1개 = 1바이트   (그대로 읽으면 됨)
    10bpp : 픽셀 4개 = 5바이트   (앞 4바이트가 상위 8비트, 마지막 1바이트에
                                  4개 픽셀의 하위 2비트가 2비트씩 모여 있음)
    12bpp : 픽셀 2개 = 3바이트   (가운데 바이트에 두 픽셀의 하위 4비트가 나뉘어 있음)
    """
    raw = np.frombuffer(data, dtype=np.uint8)
    pixels = width * height

    if bits == 8:
        if raw.size < pixels:
            return None
        return raw[:pixels].astype(np.uint16).reshape(height, width)

    if bits == 12:
        need = pixels // 2 * 3
        if raw.size < need:
            return None
        g = raw[:need].reshape(-1, 3).astype(np.uint16)
        b0, b1, b2 = g[:, 0], g[:, 1], g[:, 2]
        even = (b0 << 4) | (b1 & 0x0F)    # 첫 번째 픽셀
        odd = (b2 << 4) | (b1 >> 4)       # 두 번째 픽셀
        out = np.empty(pixels, dtype=np.uint16)
        out[0::2] = even
        out[1::2] = odd
        return out.reshape(height, width)

    if bits == 10:
        need = pixels // 4 * 5
        if raw.size < need:
            return None
        g = raw[:need].reshape(-1, 5).astype(np.uint16)
        low = g[:, 4]
        out = np.empty(pixels, dtype=np.uint16)
        for k in range(4):
            out[k::4] = (g[:, k] << 2) | ((low >> (2 * k)) & 0x03)
        return out.reshape(height, width)

    return None


def load(path, bits, width=RAW_WIDTH, height=RAW_HEIGHT):
    """파일 하나를 읽어 픽셀 배열로. 실패하면 (None, 이유)"""
    if not os.path.exists(path):
        return None, "파일이 없습니다"

    size = os.path.getsize(path)
    want = expected_file_size(bits, width, height)
    if size < want:
        return None, (f"파일이 너무 작습니다 ({size:,} < {want:,} 바이트). "
                      f"{bits}bpp 가 맞는지, 헤더가 붙어 있지 않은지 확인하세요")

    with open(path, "rb") as f:
        data = f.read()

    # 파일 앞에 헤더가 붙어 있으면 뒤쪽 want 바이트만 쓴다
    if size > want:
        data = data[size - want:]

    img = unpack(data, bits, width, height)
    if img is None:
        return None, "풀어내기에 실패했습니다"
    return img, None


# ==============================================================
# 12x12 존 통계
# ==============================================================
class ZoneStats:
    """
    존 한 칸의 통계.

    긴 노출(짝수 행)과 짧은 노출(홀수 행)을 따로 센다.
    AE는 긴 노출 밝기로만 판단하는데, 짧은 노출은 원래 몇 배~수십 배
    어두워서 섞으면 멀쩡한 칸도 "너무 어둡다"고 잘못 버리게 되기 때문이다.
    """
    __slots__ = ("long_mean", "short_mean",
                 "long_sat_ratio", "long_dark_ratio",
                 "short_sat_ratio", "short_dark_ratio")

    def __init__(self):
        self.long_mean = 0.0
        self.short_mean = 0.0
        self.long_sat_ratio = 0.0
        self.long_dark_ratio = 0.0
        self.short_sat_ratio = 0.0
        self.short_dark_ratio = 0.0


def zone_stats(img, bits):
    """
    픽셀 배열 -> 144칸 통계

    C++ ae_zone_stats.cu 커널이 GPU에서 하는 계산과 같은 일을 한다.
    (측정은 실시간이 아니라 느려도 상관없다)
    """
    pixel_max = PIXEL_MAX.get(bits, 4095)
    saturated_at = pixel_max * 98 // 100    # 98% 이상이면 하얗게 날아감
    dark_at = pixel_max * 1 // 100          # 1% 이하면 새까맣게 뭉갬

    # 가운데 2040x2040만 잘라낸다
    crop = img[CROP_OFFSET:CROP_OFFSET + CROP_SIZE,
               CROP_OFFSET:CROP_OFFSET + CROP_SIZE].astype(np.float64)

    zones = []
    for zy in range(ZONE_GRID):
        for zx in range(ZONE_GRID):
            cell = crop[zy * ZONE_SIDE:(zy + 1) * ZONE_SIDE,
                        zx * ZONE_SIDE:(zx + 1) * ZONE_SIDE]

            # 잘라낸 좌표에서의 짝/홀 행이 원본 행과 같아야 한다.
            # CROP_OFFSET 이 짝수(4)라 그대로 유지된다.
            long_rows = cell[0::2]      # 짝수 행 = 긴 노출
            short_rows = cell[1::2]     # 홀수 행 = 짧은 노출

            z = ZoneStats()
            z.long_mean = float(long_rows.mean())
            z.short_mean = float(short_rows.mean())
            z.long_sat_ratio = float((long_rows >= saturated_at).mean())
            z.long_dark_ratio = float((long_rows <= dark_at).mean())
            z.short_sat_ratio = float((short_rows >= saturated_at).mean())
            z.short_dark_ratio = float((short_rows <= dark_at).mean())
            zones.append(z)
    return zones


# ==============================================================
# 존 통계 -> 한 줄 요약 (측정 기록용)
# ==============================================================
def summarize(zones):
    """
    144칸을 숫자 몇 개로 줄인다.

    AE 본체(compute_weighted_metering)와 같은 기준으로,
    하얗게 날아갔거나 새까만 칸은 평균에서 뺀다.
    그런 칸은 노출을 바꿔도 값이 안 변해서 계산을 망치기 때문이다.
    """
    TOO_SATURATED = 0.5     # 칸의 절반 이상이 포화면 그 칸은 제외
    TOO_DARK = 0.9

    usable = [z for z in zones
              if z.long_sat_ratio < TOO_SATURATED and z.long_dark_ratio < TOO_DARK]

    if usable:
        mean_pixel = sum(z.long_mean for z in usable) / len(usable)
    else:
        # 모든 칸이 망가진 극단 상황: 방향만 알 수 있게 전체 평균
        mean_pixel = sum(z.long_mean for z in zones) / len(zones)

    # 칸 단위로 "포화된 칸이 몇 %인가"
    sat_zones = sum(1 for z in zones if z.long_sat_ratio >= TOO_SATURATED)
    dark_zones = sum(1 for z in zones if z.long_dark_ratio >= TOO_DARK)
    short_sat_zones = sum(1 for z in zones if z.short_sat_ratio >= TOO_SATURATED)

    return {
        "mean_pixel": round(mean_pixel),
        "usable_zones": len(usable),
        "saturated_pct": sat_zones * 100 // ZONE_COUNT,
        "dark_pct": dark_zones * 100 // ZONE_COUNT,
        "short_saturated_pct": short_sat_zones * 100 // ZONE_COUNT,
    }
