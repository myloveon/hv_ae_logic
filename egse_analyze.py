#!/usr/bin/env python3
"""
egse_analyze.py - EGSE 측정 결과 자동 분석

egse_tool 이 만든 egse_result.csv 를 읽어서,
사람이 판단해야 할 것들을 대신 계산해 준다.

  사용법:  python3 egse_analyze.py egse_result.csv

주의: 이 스크립트는 PC에서 도는 분석 도구다.
      카메라에 올라가는 코드가 아니므로 실수 연산을 써도 된다.
      (정수 전용 규칙은 mcamv4_ae/ 안의 C++ 코드에만 적용된다)
"""
import csv
import sys


def load(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def to_int(v, default=0):
    try:
        return int(v)
    except (TypeError, ValueError):
        return default


# ==============================================================
# [A] HDR + 포맷 호환
# ==============================================================
def analyze_A(rows):
    rows = [r for r in rows if r["item"] == "A"]
    if not rows:
        return
    print("\n" + "=" * 60)
    print("[A] HDR + 다운로드 포맷 호환")
    print("=" * 60)

    ok_combos = []
    for r in rows:
        hdr = r["hdr"] == "1"
        bits = to_int(r["data_bits"])
        ok = r["ok"] == "1"
        if hdr and ok:
            ok_combos.append(bits)

    if 12 in ok_combos:
        print("  판정: HDR + 12bpp 사용 가능")
        print("  조치: 지금 AE 설정을 그대로 쓰면 된다.")
        print("        (target_brightness 2240, saturation 4000, dark 32)")
    elif 10 in ok_combos:
        print("  판정: HDR + 12bpp 불가, 10bpp는 가능")
        print("  조치: 임계값을 1023 기준으로 다시 계산해야 한다.")
        print("        target 560 / saturation 1000 / dark 8")
    elif 8 in ok_combos:
        print("  판정: HDR + 8bpp만 가능")
        print("  조치: 임계값을 255 기준으로 다시 계산해야 한다.")
        print("        target 139 / saturation 249 / dark 2")
        print("  ※ 8bpp는 밝기 해상도가 12bpp의 1/16이라 AE 정밀도가 떨어진다.")
    else:
        print("  판정: HDR로 찍은 이미지를 내려받지 못했다.")
        print("  조치: FID 코드를 확인하고 MCSE에 문의할 것.")


# ==============================================================
# [B] 선형성 + 33us 검증  ★가장 중요★
# ==============================================================
def fit_line(xs, ys):
    """최소제곱으로 y = a*x + b 를 맞추고 (a, b, 잔차RMS)를 돌려준다."""
    n = len(xs)
    if n < 2:
        return 0.0, 0.0, float("inf")
    mx = sum(xs) / n
    my = sum(ys) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    den = sum((x - mx) ** 2 for x in xs)
    if den == 0:
        return 0.0, my, float("inf")
    a = num / den
    b = my - a * mx
    rms = (sum((y - (a * x + b)) ** 2 for x, y in zip(xs, ys)) / n) ** 0.5
    return a, b, rms


def analyze_B(rows):
    rows = [r for r in rows if r["item"] == "B" and r["ok"] == "1"]
    # 포화된 줄은 직선이 아니므로 제외한다
    rows = [r for r in rows if to_int(r["saturated_pct"]) < 50]
    if len(rows) < 4:
        return
    print("\n" + "=" * 60)
    print("[B] 선형성 + 33us 검증")
    print("=" * 60)

    ys = [to_int(r["mean_pixel"]) for r in rows]
    x_with = [to_int(r["effective_us"]) for r in rows]   # 33us 포함
    x_without = [to_int(r["nominal_us"]) for r in rows]  # 33us 제외

    a1, b1, rms1 = fit_line(x_with, ys)
    a2, b2, rms2 = fit_line(x_without, ys)

    print(f"  모델1 (33us 있음): 잔차 RMS = {rms1:8.3f}   기울기 {a1:.6f}  절편 {b1:8.2f}")
    print(f"  모델2 (33us 없음): 잔차 RMS = {rms2:8.3f}   기울기 {a2:.6f}  절편 {b2:8.2f}")

    # 잔차만으로 못 가릴 때는 절편(블랙레벨)이 물리적으로 말이 되는지 본다.
    # 33us를 빼먹으면 그만큼이 절편으로 흡수되어 블랙레벨이 부풀려진다.
    if rms1 < 1e-6 and rms2 < 1e-6:
        print("\n  두 모델 모두 완벽한 직선이다. 절편으로 판별한다.")
        print(f"    모델1 절편 {b1:.1f} / 모델2 절편 {b2:.1f}")
        if b2 > b1 * 1.5 + 1:
            print("\n  판정: 33us 모델이 맞다.")
            print(f"        33us를 빼면 절편이 {b2 - b1:.1f}만큼 부풀려진다.")
            print("        이는 33us x 기울기 와 일치해야 한다:"
                  f" {33 * a1:.1f}")
            print("  조치: 지금 AE 코드가 맞다. 그대로 두면 된다.")
        else:
            print("\n  판정: 판별 불가. 조명을 바꿔 한 번 더 측정할 것.")
        return

    if rms1 < rms2 * 0.9:
        print("\n  판정: 33us 모델이 확실히 더 잘 맞는다.")
        print("  조치: 지금 AE 코드가 맞다. 그대로 두면 된다.")
    elif rms2 < rms1 * 0.9:
        print("\n  판정: 33us 없는 모델이 더 잘 맞는다.  ★예상과 다름★")
        print("  조치: ICD의 33us 설명을 MCSE에 재확인할 것.")
        print("        코드의 kExposureOverhead 를 0으로 바꿔야 할 수도 있다.")
    else:
        print("\n  판정: 두 모델 차이가 뚜렷하지 않다.")
        print("  조치: 짧은 노출(레지스터 1~10) 데이터를 더 모을 것.")
        print("        그 구간에서 두 모델 차이가 가장 크게 벌어진다.")

    print(f"\n  블랙레벨(절편) 추정: {b1:.1f} 픽셀")
    print(f"  조도 계수(기울기)  : 실제노출 1us 당 {a1:.6f} 픽셀")


# ==============================================================
# [C] 블랙레벨 잔차
# ==============================================================
def analyze_C(rows, target=2240):
    off = [r for r in rows if r["item"] == "C_off" and r["ok"] == "1"]
    on = [r for r in rows if r["item"] == "C_on" and r["ok"] == "1"]
    if not on:
        return
    print("\n" + "=" * 60)
    print("[C] 블랙레벨 잔차 (렌즈캡)")
    print("=" * 60)

    worst = max(to_int(r["mean_pixel"]) for r in on)
    limit = target * 0.004          # 목표밝기의 0.4%

    # 다크 프레임인데 밝으면 렌즈캡을 안 씌운 것이다
    if worst > target * 0.5:
        print(f"  ★ 측정값이 너무 밝다 ({worst} 픽셀).")
        print("     렌즈캡을 안 씌웠거나 빛이 새어 들어오고 있다.")
        print("     완전히 차광한 뒤 다시 측정할 것.")
        return

    print(f"  보정 켠 상태의 최대 잔차: {worst} 픽셀")
    print(f"  무시해도 되는 기준       : {limit:.1f} 픽셀 (목표의 0.4%)")

    if worst <= limit:
        print("\n  판정: 카메라 자체 보정으로 충분하다.")
        print("  조치: BlackLevelModel 을 계속 꺼둔다. (지금 설정 그대로)")
    else:
        print("\n  판정: 잔차가 무시할 수 없다.")
        print("  조치: 온도별 계수를 구해 BlackLevelModel 에 넣는다.")
        print("        노출시간에 비례하는 성분이 있는지 확인할 것.")
        if off:
            before = max(to_int(r["mean_pixel"]) for r in off)
            print(f"        (참고: 보정 끄면 {before} -> 켜면 {worst})")


# ==============================================================
# [D] 목표 밝기
# ==============================================================
def analyze_D(rows):
    rows = [r for r in rows if r["item"] == "D" and r["ok"] == "1"]
    if not rows:
        return
    print("\n" + "=" * 60)
    print("[D] 목표 밝기 확정")
    print("=" * 60)

    # 포화 존이 20% 이하인 것만 후보로 본다
    usable = [r for r in rows if to_int(r["saturated_pct"]) <= 20]
    if not usable:
        print("  판정: 쓸 만한 후보가 없다. 더 어두운 노출로 다시 측정할 것.")
        return

    best = max(usable, key=lambda r: to_int(r["mean_pixel"]))
    value = to_int(best["mean_pixel"])
    bits = to_int(best["data_bits"])
    full = {8: 255, 10: 1023, 12: 4095}.get(bits, 4095)

    print(f"  후보 {len(usable)}개 중 가장 밝은 것: {value} 픽셀 "
          f"(전체의 {value * 100 / full:.1f}%)")
    print(f"  그때 포화 존 {best['saturated_pct']}%, 암부 존 {best['dark_pct']}%")
    print(f"\n  조치: config.target_brightness = {value}u * fx::kOne_q8;")

    ratio = value * 100 / full
    if ratio > 40:
        print(f"\n  ※ 참고: {ratio:.0f}%는 다른 오픈소스(libcamera 16~19%)보다 높다.")
        print("     우주 영상은 어두운 영역이 많아 높게 잡는 것이 맞을 수 있으나,")
        print("     실제 화질을 눈으로 확인해 볼 것.")


# ==============================================================
# [E] 짧은 노출 목표
# ==============================================================
def analyze_E(rows):
    rows = [r for r in rows if r["item"] == "E" and r["ok"] == "1"]
    if not rows:
        return
    print("\n" + "=" * 60)
    print("[E] 짧은 노출 목표 확정")
    print("=" * 60)
    print("  ※ 이 항목은 짧은 노출 채널의 포화율을 봐야 하는데,")
    print("     CSV에는 긴 노출 기준만 저장된다.")
    print("     egse_tool 화면 출력의 '판정' 열을 보고 정할 것.")
    print("\n  고르는 법: '안전'한 것 중 가장 긴 짧은노출")
    print("             (짧을수록 안전하지만 노이즈가 늘어난다)")


# ==============================================================
# [F] 노출 반영 지연
# ==============================================================
def analyze_F(rows):
    rows = [r for r in rows if r["item"] == "F" and r["ok"] == "1"]
    if len(rows) < 4:
        return
    print("\n" + "=" * 60)
    print("[F] 노출 반영 지연")
    print("=" * 60)

    # 헤더값이 지시값과 다른 경우부터 확인
    mismatch = [r for r in rows
                if to_int(r["header_exposure1"]) != to_int(r["exposure1"])]
    if mismatch:
        print(f"  ★ 헤더값 불일치 {len(mismatch)}건 - 설정이 안 먹었다!")
        for r in mismatch[:3]:
            print(f"     지시 {r['exposure1']} -> 헤더 {r['header_exposure1']}")
        return

    # 밝기가 노출을 제대로 따라가는지 확인
    exposures = [to_int(r["exposure1"]) for r in rows]
    brightness = [to_int(r["mean_pixel"]) for r in rows]

    same_row = 0     # 같은 줄에서 맞음
    shifted = 0      # 한 줄 밀림
    for i in range(1, len(rows)):
        exp_up = exposures[i] > exposures[i - 1]
        bri_up = brightness[i] > brightness[i - 1]
        if exp_up == bri_up:
            same_row += 1
        else:
            shifted += 1

    print(f"  노출 변화와 밝기 변화가 같은 줄에서 일치: {same_row}회")
    print(f"  어긋남: {shifted}회")

    if shifted == 0:
        print("\n  판정: 지연 없음. 지시한 노출이 그 사진에 바로 반영된다.")
        print("  조치: 지금 AE 구조 그대로 두면 된다.")
    else:
        print("\n  판정: ★ 한 프레임 지연이 의심된다.")
        print("  조치: AE가 이전 노출의 결과를 새 노출 결과로 착각하게 된다.")
        print("        촬영 후 한 장을 버리거나, 계산에서 한 칸 밀어야 한다.")

    # 소요 시간
    times = [to_int(r["elapsed_ms"]) for r in rows]
    print(f"\n  한 장당 소요: 최소 {min(times)}ms / 최대 {max(times)}ms")
    budget = sum(sorted(times)[:5])
    print(f"  가장 빠른 5장 합계: {budget}ms  (5fps 예산 1000ms)")
    if budget > 1000:
        print("  ★ 5장이 1초를 넘는다. 노출 상한을 더 낮춰야 한다.")


# ==============================================================
def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    rows = load(sys.argv[1])
    print(f"측정 기록 {len(rows)}줄을 읽었습니다.")

    analyze_A(rows)
    analyze_B(rows)
    analyze_C(rows)
    analyze_D(rows)
    analyze_E(rows)
    analyze_F(rows)

    print("\n" + "=" * 60)
    print("분석 끝. 위 '조치' 항목대로 코드 설정을 고치면 됩니다.")
    print("=" * 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
