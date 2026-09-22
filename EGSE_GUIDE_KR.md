# EGSE 실측 도구 사용법

카메라를 실제로 찍어보면서 **AE에 필요한 값을 재는 도구**입니다.

AE 코드에는 아직 "추측으로 넣어둔 값"이 몇 개 있습니다.
이 도구로 실측해서 확정하는 것이 목적입니다.

---

## ★ 두 가지 경로가 있습니다

EGSE Lite는 Windows 전용 프로그램으로 조작하게 되어 있는데,
우리 AE 코드는 Jetson(Linux)에서 돕니다. 서로 직접 연결되지 않습니다.

그래서 상황에 맞게 두 경로를 준비했습니다.

| | ① 카메라 직접 제어 | ② 찍어둔 RAW 파일 |
|---|---|---|
| **언어** | C++ (`egse_tool`) | **Python** (`python/`) |
| **위치** | 프로젝트 루트 | `python/` 폴더 |
| **필요한 것** | SpaceWire 통신 구현 | 저장된 RAW 파일만 |
| **측정 가능** | A~F 전부 | A~E (**F는 불가**) |
| **언제 쓰나** | SpW 드라이버가 준비된 뒤 | **지금 바로** |

**지금 당장 쓸 수 있는 건 ②번**입니다. Windows 프로그램으로 찍어서
RAW만 저장하면 PC에서 바로 분석할 수 있습니다.

> F 항목(노출 반영 지연)은 촬영 순서와 시각이 필요한데 파일에는 그 정보가
> 없습니다. 이 항목만은 카메라를 직접 제어할 때 측정해야 합니다.

---

# ② RAW 파일로 측정하기 (Python) — 지금 바로 가능

## 준비

```bash
pip install numpy
cd python/
```

## 1단계 — 무엇을 찍어야 하는지 목록 뽑기

```bash
python3 egse_raw_tool.py plan B
```

`shot_plan.csv` 가 만들어집니다.

```
# 파일명,항목,HDR,EXPOSURE1,EXPOSURE2,STEP,비트수,블랙보정,온도
shot_0001.raw,B,0,1,0,10us,12,1,25
shot_0002.raw,B,0,2,0,10us,12,1,25
shot_0003.raw,B,0,3,0,10us,12,1,25
...
```

여러 항목을 한 번에 뽑을 수도 있습니다.

```bash
python3 egse_raw_tool.py plan B C E
python3 egse_raw_tool.py plan all
```

## 2단계 — 목록대로 촬영

Windows EGSE 프로그램에서 목록의 조건대로 찍고 RAW로 저장합니다.

| 목록 칸 | EGSE 프로그램에서 설정할 것 |
|---|---|
| HDR | TC(222,1) 의 HDR |
| EXPOSURE1 / EXPOSURE2 | TC(222,1) 의 노출값 |
| STEP | EXPOSURE_STEP (10us / 1ms) |
| 비트수 | TC(222,4) 의 IMAGE_DATA_FORMAT (8/10/12) |
| 블랙보정 | BLACK_COL_CORR |

> ⚠️ **반드시 RAW(원본)로 저장하세요.** jpg/png는 픽셀값이 손상되어 측정이 무의미해집니다.
>
> ⚠️ **전체 프레임(2048×2048)으로 받으세요.** 오프라인 측정은 시간 제약이
> 없으므로 ROI로 자를 이유가 없고, 자르면 존 격자 계산이 안 맞습니다.

## 3단계 — 파일명 채우기

저장한 실제 파일명으로 맨 앞 칸을 바꿉니다. 온도를 알면 마지막 칸도 적어둡니다.

## 4단계 — 측정

```bash
python3 egse_raw_tool.py run B shot_plan.csv raw/
#                        ^항목 ^목록파일      ^RAW 폴더
```

```
=== [B] ===
     레지스터    명령(us)    실제(us)      평균밝기     포화%     암부%
        1        10        43        29      0%      0%
        5        50        83        49      0%      0%
      500      5000      5033      2524      0%      0%
     1000     10000     10033      4095    100%      0%
```

## 5단계 — 분석

```bash
python3 egse_analyze.py egse_result.csv
```

```
판정: 33us 모델이 맞다.
      33us를 빼면 절편이 16.5만큼 부풀려진다.
      이는 33us x 기울기 와 일치해야 한다: 16.5
조치: 지금 AE 코드가 맞다. 그대로 두면 된다.
```

## RAW 파일 규격 확인

파일 크기로 형식이 맞는지 먼저 확인할 수 있습니다.

| 포맷 | 담는 방식 | 2048×2048 크기 |
|---|---|---|
| 8bpp | 픽셀 1개 = 1바이트 | 4,194,304 |
| 10bpp | 픽셀 4개 = 5바이트 | 5,242,880 |
| **12bpp** | 픽셀 2개 = 3바이트 | **6,291,456** |

크기가 다르면 도구가 이유를 알려줍니다. 파일 앞에 헤더가 붙어 있으면
자동으로 뒤쪽 실제 데이터만 읽습니다.

## Python 파일 구성

| 파일 | 역할 |
|---|---|
| `egse_raw.py` | RAW 읽기, 8/10/12bpp 풀어내기, 12×12 존 통계 |
| `egse_raw_tool.py` | 촬영 계획 만들기 + 측정 실행 |
| `egse_analyze.py` | 측정 결과 분석 + 판정 (두 경로 공용) |

---

# ① 카메라 직접 제어 (C++)

## 켜고 끄기

**기본은 꺼져 있습니다.** 비행 소프트웨어에는 들어가지 않습니다.

```bash
# 평소 (EGSE 도구 없음)
cmake ..

# 실측할 때만
cmake -DAE_ENABLE_EGSE_TOOLS=ON ..
make
```

직접 컴파일한다면:

```bash
g++ -std=c++17 -O2 -I. -DAE_ENABLE_EGSE_TOOLS -o egse_tool egse_tool.cpp
```

`AE_ENABLE_EGSE_TOOLS`를 정의하지 않으면 EGSE 코드는 **한 줄도 컴파일되지 않습니다.**
(`ae_egse_camera.hpp`, `ae_egse_measure.hpp` 전체가 `#ifdef`로 감싸져 있습니다)

---

## 실행

```bash
./egse_tool A      # 항목 A만
./egse_tool all    # 전부

python3 egse_analyze.py egse_result.csv   # 결과 자동 분석
```

---

## 실제 카메라 연결하기

`egse_tool.cpp`의 `SimulatedCamera`를 실제 통신 코드로 바꾸면 됩니다.

```cpp
class RealCamera : public ae::egse::IEgseCamera {
 public:
  ae::egse::ShotResult shoot(const ae::egse::ShotSetup& setup) override {
    ShotResult out;

    // 1) TC(222,1) Take Image
    //      HDR             = setup.hdr_enabled
    //      EXPOSURE1       = setup.exposure1
    //      EXPOSURE2       = setup.exposure2
    //      EXPOSURE_STEP   = setup.step
    //      BLACK_COL_EN    = setup.black_col_en
    //      SENSOR_RST_CFG  = setup.sensor_rst_cfg
    //
    //    실패하면 out.fid 에 FID 코드를 넣고 out.success=false 로 돌려준다

    // 2) TC(222,4) Download Image
    //      IMAGE_DATA_FORMAT = setup.data_format   (0/1/2 = 8/10/12bpp)
    //      BLACK_COL_CORR    = setup.black_col_corr
    //      ROI_SIZE_X/Y      = setup.roi_size

    // 3) 받은 RAW로 존 통계 계산 (ae_zone_stats.cu 커널)
    //      -> out.zones

    // 4) TM(3,25) HK Report
    //      -> out.sensor_temp_c_q8

    // 5) IMAGE_HEADER 되읽기  ★F 항목에 꼭 필요★
    //      -> out.header_exposure1 / header_exposure2

    // 6) 걸린 시간 측정
    //      -> out.elapsed_ms

    return out;
  }
};
```

`main()`에서 한 줄만 바꾸면 됩니다.

```cpp
// SimulatedCamera camera(500u);
RealCamera camera;
```

---

## 측정 항목

### A. HDR + 포맷 호환 (5분) ★제일 먼저★

**무엇을 확인하나**: HDR을 켠 채로 12bpp로 내려받을 수 있는가?

**왜 먼저 하나**: 안 되면 8bpp로 받아야 하고, 그러면 AE 임계값을 **전부 다시 계산**해야 합니다. 이후 모든 측정의 전제가 바뀝니다.

**준비**: 특별한 준비 없음

**판정**

| 결과 | 조치 |
|---|---|
| HDR + 12bpp 성공 | 지금 설정 그대로 |
| 12bpp 실패, 10bpp 성공 | target 560 / sat 1000 / dark 8 |
| 8bpp만 성공 | target 139 / sat 249 / dark 2 |

`FID 0xDE0A`(INCOMPATIBLE_PARAM)가 나오면 그 조합은 쓸 수 없습니다.

---

### B. 선형성 + 33µs 검증 (30분)

**무엇을 확인하나**: AE의 핵심 전제가 맞는지.

```
밝기 = k × (레지스터 × STEP + 33µs)
                              ^^^^^ 이게 진짜 있는가?
```

**준비**
- 균일한 조명면 (적분구 또는 균일 확산판)
- 조명을 **측정 내내 절대 바꾸지 않을 것**
- HDR은 끄고 측정합니다 (변수를 하나만 두기 위해)

**분석 방법**: 두 모델로 직선을 맞춰보고 잔차가 작은 쪽이 정답입니다.

```
모델1 (33µs 있음): 밝기 = k × (레지스터×STEP + 33) + b
모델2 (33µs 없음): 밝기 = k × (레지스터×STEP)      + b
```

레지스터 1~10 구간에서 확연히 갈립니다. 레지스터 1이면 명령 10µs 대 실제 43µs로 4배 차이납니다.

**덤으로 얻는 것**: 블랙레벨(절편), 조도 계수(기울기), 포화 시작점

---

### C. 블랙레벨 잔차 (반나절)

**무엇을 확인하나**: 카메라 자체 보정으로 충분한가?

**준비**
- ★ **렌즈캡 반드시 장착** (또는 완전 암실)
- 열챔버가 있으면 −20 / 0 / +25 / +45°C 스윕

**판정**

| 잔차 | 조치 |
|---|---|
| 9픽셀 미만 | `BlackLevelModel` 계속 꺼둠 ✅ |
| 그 이상 | 온도별 계수를 구해 넣어야 함 |

분석기가 값이 너무 밝으면 **"렌즈캡 안 씌웠다"고 알려줍니다.**

---

### D. 목표 밝기 확정 (1~2시간)

**무엇을 확인하나**: AE가 맞춰야 할 밝기. 지금은 2240(54.7%)인데, 다른 오픈소스는 16~19%를 씁니다. **약 3배 차이라 확인이 꼭 필요합니다.**

**준비**: 실제 운용과 비슷한 장면 (밝은 곳 + 어두운 곳 공존)

**고르는 기준** — 두 제약을 동시에 만족하는 범위의 **위쪽**

```
아래 제한: 어두운 곳이 노이즈에 묻히지 않아야 한다
위 제한  : 포화 존이 10~20%를 넘으면 안 된다
           (포화 존은 계산에서 빠지므로 너무 많으면 제어 불안정)
```

---

### E. 짧은 노출 목표 확정 (1시간)

**무엇을 확인하나**: HDR 짧은 노출을 얼마로 고정할지. 지금 2000µs는 예시값입니다.

**준비**: 밝은 광원이 포함된 장면

**고르는 기준**: 짧은 노출 채널 포화율이 1% 이하인 것 중 **가장 긴** 값
(짧을수록 안전하지만 노이즈가 늘어납니다)

---

### F. 노출 반영 지연 (15분) ★짧지만 치명적★

**무엇을 확인하나**: 지시한 노출이 **그 사진에** 반영되는가, **다음 사진부터** 반영되는가?

한 장 늦게 반영된다면, AE는 이전 노출로 찍힌 사진을 새 노출 결과로 착각합니다. **계산이 근본적으로 틀어집니다.**

**판정**

| 결과 | 의미 |
|---|---|
| 밝기가 같은 줄에서 바뀜 | 지연 없음 (정상) |
| 밝기가 한 줄 뒤에서 바뀜 | 1프레임 지연 → 보정 필요 |
| 헤더값이 지시값과 다름 | 설정이 아예 안 먹음 |

**같이 확인**: `SENSOR_RST_CFG` 때문에 한 장당 20ms가 더 걸린다고 문서에 적혀 있습니다. 5장이면 100ms라 200ms 예산에서 무시할 수 없습니다.

---

## 권장 순서

| 순위 | 항목 | 소요 | 이유 |
|---|---|---|---|
| 1 | **A** | 5분 | 결과에 따라 이후 계획이 통째로 바뀜 |
| 2 | **B** | 30분 | AE 핵심 전제 검증 |
| 3 | **F** | 15분 | 짧지만 틀리면 근간이 흔들림 |
| 4 | D | 1~2시간 | 화질 직결, 장면 준비 필요 |
| 5 | E | 1시간 | 하이라이트 장면 준비 필요 |
| 6 | C | 반나절 | 온도 스윕 시 열챔버 필요 |

**A·B·F는 하루면 끝나고, 이것만으로도 코드 확정에 필요한 핵심은 다 나옵니다.**

---

## 장비 구성 (User's Manual §11.2)

```
PC(Windows) ──USB3.0──┐
                      │  EGSE Lite  ┌─A0 (MDM9,  SpW)──→ MCAMv4 J01
                      └─────────────┤
                                    └─A11 (MDM15, 5V)──→ MCAMv4 J02
```

### ⚠️ 전원 여유 확인

| 항목 | 값 | 5V 환산 |
|---|---|---|
| EGSE Lite 공급 | 0.5 A | 2.5 W |
| 카메라 Snapshot **peak** | 4.2 W (worst) | **0.84 A** |

readout 순간 전류가 EGSE 정격을 넘습니다. HK에 `OC_xxx`(과전류)나 `FPGA_PLL_UNLOCK`이 뜨면 **§11.3 외부 PSU 구성**(5V, 1A 제한)으로 전환하세요.

### 주의사항

- ESD 보호 구역, 손목 스트랩 필수
- SpW는 LVDS라 공통모드 전압에 민감 → Figure 14 접지도 **반드시** 준수
- 연결 순서: USB → 전원(A11) → SpW(A0) → PC. 끌 때는 역순
- EGSE Lite는 **스냅샷 모드만** 지원 (비디오 불가, 우리 AE는 스냅샷이라 무관)

---

## 결과 파일

`egse_result.csv`에 모든 측정이 한 줄씩 저장됩니다.

```csv
item,exposure1,exposure2,nominal_us,effective_us,data_bits,hdr,
black_corr,mean_pixel,saturated_pct,dark_pct,temp_c,
header_exposure1,elapsed_ms,ok,fid
```

`egse_analyze.py`가 이 파일을 읽어 **"조치: 무엇을 어떻게 바꿔라"**까지 알려줍니다.
