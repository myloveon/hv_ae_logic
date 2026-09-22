#!/usr/bin/env python3
"""
egse_raw_tool.py — 저장해둔 RAW 파일로 AE 측정하기

┌──────────────────────────────────────────────────────────────┐
│ 이 도구가 필요한 이유                                          │
│                                                              │
│   EGSE Lite 는 Windows 전용 프로그램으로 조작하게 되어 있는데,  │
│   우리 AE 코드는 Jetson(Linux)에서 돈다. 서로 직접 연결이 안 된다. │
│                                                              │
│   그래서 찍는 것과 분석하는 것을 나눈다.                        │
│                                                              │
│     [Windows]  EGSE 프로그램으로 촬영  ->  RAW 파일 저장        │
│                                              │                │
│                                              ▼                │
│     [PC]       이 도구로 읽어서 분석  ->  "이렇게 바꿔라"       │
└──────────────────────────────────────────────────────────────┘

쓰는 순서
  1) python3 egse_raw_tool.py plan B              -> 찍을 목록 만들기
  2) (EGSE 프로그램에서 목록대로 촬영, RAW 저장)
  3) 목록의 파일명 칸을 실제 저장한 이름으로 수정
  4) python3 egse_raw_tool.py run B shot_plan.csv raw/   -> 측정
  5) python3 egse_analyze.py egse_result.csv             -> 판정

필요한 것
  pip install numpy
"""

import csv
import os
import sys

import egse_raw


# ==============================================================
# 센서 규격 (ICD)
# ==============================================================
SENSOR_OVERHEAD_US = 33     # 실제 노출 = 레지스터 x STEP + 33us


def step_us(step_name):
    return 1000 if step_name == "1ms" else 10


# ==============================================================
# 촬영 계획 — 각 측정 항목마다 어떤 조건으로 찍어야 하는지
#
#   C++ egse_tool 의 측정 스윕과 같은 값이어야
#   두 경로의 결과를 비교할 수 있다.
# ==============================================================
def plan_A():
    """HDR + 다운로드 포맷 호환 확인"""
    rows = []
    for hdr in (1, 0):
        for bits in (12, 10, 8):
            rows.append(dict(item="A", hdr=hdr, e1=1000, e2=100,
                             step="10us", bits=bits, black_corr=1))
    return rows


def plan_B():
    """선형성 + 33us 검증 (HDR 끄고 단일 노출)

    33us 는 짧은 노출에서만 구분된다.
      레지스터   1 : 명령 10us  vs 실제 43us   -> 330% 차이 (매우 좋음)
      레지스터  20 : 명령 200us vs 실제 233us  -> 16% 차이  (좋음)
      레지스터 100 : 명령 1ms   vs 실제 1.03ms -> 3% 차이   (거의 불가)
    그래서 짧은 쪽을 촘촘히 찍는다.
    """
    sweep = [1, 2, 3, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000]
    return [dict(item="B", hdr=0, e1=v, e2=0, step="10us", bits=12, black_corr=1)
            for v in sweep]


def plan_C():
    """블랙레벨 잔차 — 렌즈캡 씌우고, 보정 끈 것과 켠 것을 모두"""
    rows = []
    for v in (10, 100, 1000, 10000):
        rows.append(dict(item="C_off", hdr=0, e1=v, e2=0,
                         step="10us", bits=12, black_corr=0))
        rows.append(dict(item="C_on", hdr=0, e1=v, e2=0,
                         step="10us", bits=12, black_corr=1))
    return rows


def plan_D():
    """목표 밝기 확정 — 실제 운용과 비슷한 장면에서"""
    sweep = [50, 100, 200, 400, 800, 1600, 3200, 6400, 10000]
    return [dict(item="D", hdr=1, e1=v, e2=max(1, v // 8),
                 step="10us", bits=12, black_corr=1) for v in sweep]


def plan_E(fixed_long=4000):
    """짧은 노출 목표 확정 — 긴 노출 고정, 짧은 것만 바꿔가며"""
    sweep = [1, 2, 5, 10, 25, 50, 100, 200, 400]
    return [dict(item="E", hdr=1, e1=fixed_long, e2=v,
                 step="10us", bits=12, black_corr=1)
            for v in sweep if v < fixed_long]


PLANS = {"A": plan_A, "B": plan_B, "C": plan_C, "D": plan_D, "E": plan_E}

# F 항목(노출 반영 지연)은 파일로 측정할 수 없다.
# "지시한 노출이 그 사진에 반영됐는지"는 촬영 순서와 시각이 필요한데
# 파일에는 그 정보가 없기 때문이다. 카메라를 직접 제어할 때 측정해야 한다.


# ==============================================================
# 계획 파일 만들기
# ==============================================================
def make_plan(items, out_path="shot_plan.csv"):
    rows = []
    for name in items:
        if name not in PLANS:
            continue
        rows.extend(PLANS[name]())

    if not rows:
        print("만들 계획이 없습니다.")
        return False

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("# EGSE 촬영 계획\n")
        f.write("#\n")
        f.write("# 이 목록대로 EGSE 프로그램에서 촬영하고 RAW 로 저장한 뒤,\n")
        f.write("# 맨 앞 칸(파일명)을 실제 저장한 이름으로 바꿔주세요.\n")
        f.write("#\n")
        f.write("# * 반드시 RAW(원본)로 저장하세요. jpg/png 는 픽셀값이 손상됩니다.\n")
        f.write("# * 전체 프레임(2048x2048)으로 받으세요. ROI 로 자르면 안 됩니다.\n")
        f.write("#   (오프라인 측정은 시간 제약이 없으므로 전체를 받아도 됩니다)\n")
        f.write("#\n")
        f.write("# 파일명,항목,HDR,EXPOSURE1,EXPOSURE2,STEP,비트수,블랙보정,온도\n")
        for i, r in enumerate(rows, 1):
            f.write(f"shot_{i:04d}.raw,{r['item']},{r['hdr']},{r['e1']},{r['e2']},"
                    f"{r['step']},{r['bits']},{r['black_corr']},25\n")

    print(f"촬영 계획 {len(rows)}장을 {out_path} 에 저장했습니다.")
    print()
    print("항목별 장수:")
    for name in items:
        if name in PLANS:
            print(f"  {name}: {len(PLANS[name]())}장")
    if "F" in items:
        print("  F: 파일 방식으로는 측정할 수 없습니다 (카메라 직접 제어 필요)")
    return True


# ==============================================================
# 계획 파일 읽기
# ==============================================================
def read_plan(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line_no, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split(",")]
            if len(parts) < 8:
                print(f"  [무시] {line_no}번째 줄을 이해할 수 없습니다")
                continue
            try:
                rows.append({
                    "file": parts[0],
                    "item": parts[1],
                    "hdr": int(parts[2]),
                    "e1": int(parts[3]),
                    "e2": int(parts[4]),
                    "step": parts[5],
                    "bits": int(parts[6]),
                    "black_corr": int(parts[7]),
                    "temp": int(parts[8]) if len(parts) > 8 else 25,
                })
            except ValueError:
                print(f"  [무시] {line_no}번째 줄에 숫자가 아닌 값이 있습니다")
    return rows


# ==============================================================
# 측정 실행
# ==============================================================
def run(items, plan_path, base_dir, out_path="egse_result.csv"):
    rows = read_plan(plan_path)
    if not rows:
        print("계획 파일이 비어 있습니다.")
        return False

    wanted = set(items) if "all" not in items else None
    results = []
    missing = 0

    print(f"계획 {len(rows)}장을 읽었습니다.\n")

    for r in rows:
        # C 항목은 C_off / C_on 두 갈래라 앞글자로 비교한다
        base_item = r["item"].split("_")[0]
        if wanted is not None and base_item not in wanted:
            continue

        path = r["file"] if os.path.isabs(r["file"]) else os.path.join(base_dir, r["file"])
        img, err = egse_raw.load(path, r["bits"])

        nominal = r["e1"] * step_us(r["step"])
        effective = nominal + SENSOR_OVERHEAD_US

        if img is None:
            missing += 1
            if missing <= 5:
                print(f"  [건너뜀] {r['file']}: {err}")
            results.append(dict(item=r["item"], e1=r["e1"], e2=r["e2"],
                                nominal=nominal, effective=effective,
                                bits=r["bits"], hdr=r["hdr"],
                                black_corr=r["black_corr"],
                                mean_pixel=0, sat=0, dark=0, short_sat=0,
                                temp=r["temp"], ok=0))
            continue

        zones = egse_raw.zone_stats(img, r["bits"])
        s = egse_raw.summarize(zones)

        results.append(dict(item=r["item"], e1=r["e1"], e2=r["e2"],
                            nominal=nominal, effective=effective,
                            bits=r["bits"], hdr=r["hdr"],
                            black_corr=r["black_corr"],
                            mean_pixel=s["mean_pixel"],
                            sat=s["saturated_pct"], dark=s["dark_pct"],
                            short_sat=s["short_saturated_pct"],
                            temp=r["temp"], ok=1))

    if not results:
        print("측정할 것이 없습니다. 항목 이름을 확인하세요.")
        return False

    write_result(results, out_path)
    show_table(results)

    ok = sum(1 for r in results if r["ok"])
    print(f"\n총 {len(results)}건 중 {ok}건 성공. {out_path} 저장 완료.")
    if missing:
        print(f"{missing}건은 파일을 읽지 못했습니다.")
    print(f"\n다음: python3 egse_analyze.py {out_path}")
    return True


def write_result(results, path):
    """C++ egse_tool 과 같은 형식으로 저장한다 (같은 분석기를 쓰기 위해)"""
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["item", "exposure1", "exposure2", "nominal_us", "effective_us",
                    "data_bits", "hdr", "black_corr", "mean_pixel",
                    "saturated_pct", "dark_pct", "temp_c",
                    "header_exposure1", "elapsed_ms", "ok", "fid"])
        for r in results:
            w.writerow([r["item"], r["e1"], r["e2"], r["nominal"], r["effective"],
                        r["bits"], r["hdr"], r["black_corr"], r["mean_pixel"],
                        r["sat"], r["dark"], r["temp"],
                        r["e1"],      # 파일 방식은 지시한 대로 찍혔다고 본다
                        0,            # 소요시간은 알 수 없다
                        r["ok"], "0x0000"])


def show_table(results):
    """측정 결과를 항목별로 보여준다"""
    by_item = {}
    for r in results:
        by_item.setdefault(r["item"], []).append(r)

    for item, rows in by_item.items():
        print(f"\n=== [{item}] ===")
        if item == "E":
            print(f"{'짧은노출':>9}{'실제(us)':>10}{'비율':>8}"
                  f"{'short포화%':>11}  판정")
            for r in rows:
                if not r["ok"]:
                    print(f"{r['e2']:>9}  (파일 없음)")
                    continue
                ratio = r["e1"] // max(1, r["e2"])
                safe = "안전" if r["short_sat"] <= 1 else "포화 발생"
                print(f"{r['e2']:>9}{r['effective']:>10}{ratio:>7}:1"
                      f"{r['short_sat']:>10}%  {safe}")
        else:
            print(f"{'레지스터':>9}{'명령(us)':>10}{'실제(us)':>10}"
                  f"{'평균밝기':>10}{'포화%':>8}{'암부%':>8}")
            for r in rows:
                if not r["ok"]:
                    print(f"{r['e1']:>9}  (파일 없음)")
                    continue
                print(f"{r['e1']:>9}{r['nominal']:>10}{r['effective']:>10}"
                      f"{r['mean_pixel']:>10}{r['sat']:>7}%{r['dark']:>7}%")


# ==============================================================
def usage():
    print(__doc__)
    print("명령:")
    print("  plan <항목...>                       찍을 목록 만들기")
    print("  run  <항목...> <목록파일> [RAW폴더]   측정 실행")
    print()
    print("항목:")
    print("  A   HDR + 다운로드 포맷 호환      (5분)   ★제일 먼저")
    print("  B   선형성 + 33us 검증            (30분)")
    print("  C   블랙레벨 잔차 (렌즈캡 필요)   (반나절)")
    print("  D   목표 밝기 확정                (1~2시간)")
    print("  E   짧은 노출 목표 확정           (1시간)")
    print("  all 전부")
    print()
    print("  ※ F(노출 반영 지연)은 파일로 측정 불가 — 카메라 직접 제어 필요")
    print()
    print("예)")
    print("  python3 egse_raw_tool.py plan B")
    print("  python3 egse_raw_tool.py run B shot_plan.csv raw/")


def main():
    if len(sys.argv) < 3:
        usage()
        return 1

    cmd = sys.argv[1]

    if cmd == "plan":
        items = sys.argv[2:]
        if "all" in items:
            items = ["A", "B", "C", "D", "E"]
        return 0 if make_plan(items) else 1

    if cmd == "run":
        args = sys.argv[2:]
        # 마지막 인자들 중 파일/폴더를 찾아낸다
        plan_path = None
        base_dir = "."
        items = []
        for a in args:
            if plan_path is None and a.endswith(".csv"):
                plan_path = a
            elif plan_path is not None:
                base_dir = a
            else:
                items.append(a)
        if plan_path is None:
            print("목록 파일(.csv)을 지정해주세요.\n")
            usage()
            return 1
        if not items:
            items = ["all"]
        return 0 if run(items, plan_path, base_dir) else 1

    usage()
    return 1


if __name__ == "__main__":
    sys.exit(main())
