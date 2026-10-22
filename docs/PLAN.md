# EO-IC100 × Nintendo Switch — 진행 계획

목표: 탈옥(Atmosphère) 스위치에서 삼성 정품 USB-C 이어폰 **EO-IC100**(`04e8:a054`, 디지털 UAC1)으로 소리를 낸다.

이번 작업 범위는 원인을 확정하기 위한 **진단 도구**와 **접근 A 프로토타입**(전송 자체가 되는지)이다.
게임 소리 출력(접근 B/C)은 이 결과를 보고 다음에 진행한다.

---

## 0. 실기기 결과 (2026-10-20, HOS 22.1.0) — 접근 A 성공

| 단계 | 결과 |
|---|---|
| Step 1 (PC) | 사용자의 기기는 **`04e8:a05b`** 이다. 문자열은 "Samsung" / "Samsung USB C Earphones", full-speed, config 345바이트로, 공개된 a054(305바이트)와 펌웨어가 다르다. 재생 경로는 IT4 → FU5 → OT6(0x0402)이고, 48/96 kHz만 지원하며 32-bit alt3가 추가되어 있다. 가설 플래그 H1~H7은 a054와 같다. Windows 오디오는 정상이지만 **MI_04(IF4) HID만 Code 10**이다 |
| Code 10 원인 | IF4 HID 리포트 디스크립터의 `Logical Maximum`이 `25 FF`(1바이트 부호 있음 = **−1**)라서 Minimum 0보다 작다. 규격 위반이라 Windows hidparse가 거부한다. Usage Page 0xFEFF도 벤더 영역(0xFF00+)이 아닌 예약 영역이다. `uacdesc.py hid`가 둘 다 검출한다 |
| Step 2 (스위치) | usb:pm 결과 스위치는 **SOURCE + DFP(호스트)** 로 정상이다. **IF0만 available이고 IF1·IF2(AS)는 audio sysmodule이, IF3·IF4(HID)는 hid sysmodule로 보이는 프로세스가 점유**하고 있었다. 그런데도 auddev 출력 목록에 USB 장치가 없다. 즉 오디오 드라이버가 스트리밍 인터페이스는 잡았지만 출력 장치를 만들지 못하고 있다 |
| race-acquire | 재연결 순간 IF1→IF2→IF0→IF3→IF4를 **모두 +0 ms에 획득**했다. 스위치에서 읽은 config는 PC 덤프와 바이트 단위로 같고, HID 리포트 디스크립터도 확보했다 |
| UAC 제어 | SET_INTERFACE alt1, SET_CUR 48000(GET_CUR로 48000 확인), FU5 볼륨(범위 −60~0 dB, 0.5 dB 단위, 초기값 −4 dB), 음소거 해제가 모두 OK |
| Step 3 (스윕) | **`[4] batch 0/frame` → `PACED 192.0 kB/s, err 0`**: 리포트 1492개가 모두 성공했고 4 ms마다 정확히 완료됐다. 사용자는 **삐 소리를 들었다** |

**isoc 호출 규약(확정):** `usbHsEpBatchBufferAsync(ep, buf, urbs[], n, id, unk1 = 0, unk2 = 시작 프레임)`
- `unk2`는 **절대 11비트 USB 프레임 번호**다. GetCurrentFrame은 1 ms에 1씩 증가하고 2048에서 한 바퀴 돈다. `현재 + 16`부터 배치마다 urb 수만큼 더하면 끊김 없이 이어진다.
- `0/0`(= PostBufferAsync)은 "프레임 0"으로 예약된다. 카운터가 한 바퀴 돌 때까지(838 ms) 기다렸다가 한 배치만 성공하는 것이 관측되어 해석이 확인됐다.
- 보고는 urb마다 1개씩 온다(배치 4 urb → 리포트 4개).
- `0xD28C`(2140-0105): 예약 창을 벗어난 프레임이다(약 900프레임 이상 앞서 있거나 이미 지났음). 즉시 반환된다. `0x32A8C`(2140-0405): 이미 지나간/겹친 프레임에 대한 전송 실패다.
- eoprobe v0.2.0부터 이 방식이 기본 전략(0번)이다. 실패하면 GetCurrentFrame으로 프레임을 재동기화하고, X는 B를 누를 때까지(최대 60초) 연속 재생한다.

**의미:** 하드웨어, USB 경로, isoc 전송 모두 스위치에서 동작한다. 남은 문제는 **닌텐도 오디오 드라이버의 소프트웨어 판정**뿐이다(H1~H7 중 무엇이 원인인지). 이 문제는 C-0 → C로 해결할 수 있는 종류다.

**v0.2.0 재시험 (같은 날 17:16):**
- **연속 재생 16초**(4,036 배치): 오류 0, 재동기화 0, 최대 간격 4.2 ms. L/R 패턴 6초도 오류 0. 채널 순서는 들은 결과가 아직 없다.
- **리모컨**: IF3 리포트 `01 01 00`=Vol+, `01 02 00`=Vol−, `01 00 00`=뗌. HID 디코드와 일치한다. Play/Pause(`01 04 00` 예상)는 아직 시험하지 않았다.
- **점유 상태는 게임 실행 여부에 따라 달랐다**: 게임을 종료한 상태에서는 앱 시작 스캔에서 IF0~IF4가 **모두 available**이었고, race 없이 바로 확보됐다. 게임이 실행(일시정지) 중이던 앞선 실행에서는 IF1~IF4가 점유돼 있었다. 즉 sysmodule들은 **게임(애플리케이션)이 있을 때만** 이 장치를 잡는 것으로 보인다. C-0 로깅은 게임을 실행한 상태에서 해야 의미가 있다.
- **재열거 문제**: PC(MTP)에서 뽑고 바로 이어폰을 꽂은 경우, race로 잡은 장치(dev 8)가 몇 초 안에 다시 열거되어(→ dev 11) 시스템이 가져갔다. USB-C 역할 전환으로 추정된다. v0.2.1부터 race 모드는 재열거를 감지하면 계속 경주하고, 3초간 안정적으로 쥐고 있어야 끝난다.

**v0.2.1 게임 실행 중 시험 (17:23~17:25):**
- **채널 순서 정상**: 사용자가 좌 → 우 순서로 들었다(v0.2.0과 v0.2.1 모두).
- **게임 실행 중**: 이어폰이 연결되는 순간 IF1~IF4가 sysmodule에 점유됐다. 그래도 R(race) → X로 **톤이 정상 재생**됐다(오류 0, 192 kB/s). 게임 소리는 계속 콘솔 스피커로 나왔다. 즉 우리 스트림은 시스템 오디오와 공존하고, 시스템은 이어폰으로 소리를 보내지 않는다. 접근 B의 전송 측 전제가 충족됐다.
- **sysmodule은 연결 순간에만 점유한다**: 우리가 쥐고 있던 인터페이스를 앱 종료로 놓아준 뒤 다시 실행하자 5개 모두 available이었다. 다시 꽂아야 시스템이 잡는다.
- **꽂을 때마다 약 3회 재열거된다**: 장치 번호가 18→21, 21→24, 25→28로 3씩 증가했다. 마지막 race에서는 +933 ms에 재열거, +4704 ms에 다시 획득했다. v0.2.1의 재획득 로직이 동작했다. v0.2.2는 안정 판정 시간을 6초로 늘렸다.
- **버그 수정(v0.2.2)**: 한 번은 첫 전송이 목표 프레임을 놓쳤다(0xD28C 4회 후 재동기화 1회로 복구). 프레임 번호를 읽은 뒤 첫 전송 전에 SD 로그 쓰기(약 30 ms)가 끼어 있던 것이 원인이다. 이제 스트림이 끝날 때까지 SD에 쓰지 않고, 첫 전송 직전에 프레임을 다시 읽는다.
- **로그**: v0.2.2부터 실행마다 `logs/eoprobe_YYYYMMDD_HHMMSS.txt` 새 파일에 기록한다(사용자 요청).

**C-0 결과 (2026-10-22, usbhs-logmitm v0.1.0, 로그 `dumps/usbhslog/usbhslog_20261022_092946.txt`):** 1단계(eoprobe 톤)는 소리가 났다. 2단계에서는 게임 중 이어폰을 꽂아도 소리가 본체 스피커에서만 났다. MITM은 sm fatal 없이 동작했고 메모리 읽기 실패도 0이었다.
- **audio는 usb:hs를 필요할 때 연다.** 부팅 때(+16 s) 세션을 열어 조회 결과 0개를 받고 20초 뒤 닫았다. 13:10:06(슬립에서 깬 뒤, 게임 실행 무렵)에 새 세션을 열었다. 그래서 mitm.lst 없이도 audio를 가로챌 수 있을 가능성이 높다(다음 시험에서 확인).
- audio의 usb:hs 세션은 **CMIF 도메인**이다(ConvertCurrentObjectToDomain, IF·EP도 도메인 객체).
- **오디오 드라이버의 순서(꽂았을 때, 13:10:28):**
  1. `CreateInterfaceAvailableEvent`(필터 bInterfaceClass=01) → `QueryAvailableInterfaces` → IF0(AC), IF1, IF2(AS) 3개
  2. IF0 Acquire → `GET_DESCRIPTOR CONFIG len 345`(PC 덤프와 같은 바이트) → **IF0 놓음**
  3. IF1 Acquire → 같은 GET_DESCRIPTOR → 놓음 → **다시 Acquire → SetInterface(alt 0)** 후 계속 쥠
  4. IF2도 같은 순서로 다시 Acquire → SetInterface(alt 0) 후 계속 쥠
  5. **그 뒤 아무 요청도 없다.** SET_INTERFACE alt 1, SET_CUR(샘플레이트), FU 볼륨, OpenUsbEp가 하나도 없었다. 실패한 요청도 없다. 1분 뒤(13:11:28, 뽑음) 두 IF 객체를 닫았다.
- **해석:** USB 요청이 실패해서 멈춘 것이 아니다. 드라이버는 디스크립터를 읽고 AS 인터페이스 2개를 "쓸 수 있다"고 보고 쥐었다. 그런데 **출력 장치로 등록하지 않았다.** 이것은 디스크립터 해석 결과에 따른 분류 단계의 판정이다. H5·H7은 해당이 적다(IF3/IF4는 audio가 조회조차 하지 않는다). H1(마이크가 먼저)도 두 AS를 모두 처리했으니 가능성이 낮다. **가장 유력한 것은 H2**: 재생 경로 OT6과 마이크 경로 IT1의 wTerminalType이 모두 0x0402(Headset, 양방향 타입)이다. 출력 단자를 0x03xx(Speaker/Headphones)에서 찾는 구현이라면 "출력 없음"이 된다. 다음 후보는 H3(재생 alt1에 48/96 kHz 두 레이트, wMaxPacketSize 384)과 alt2·alt3(24·32비트)이다.
- **C의 경로가 열렸다.** 디스크립터는 audio 메모리로 들어가지만 `svcReadDebugProcessMemory`가 실기기에서 동작했다(audio에 이상 없음). 같은 방식의 `svcWriteDebugProcessMemory`로 GetCtrlXferReport를 돌려주기 전에 디스크립터를 고쳐 쓰면 가설을 하나씩 시험할 수 있다. 첫 시험은 H2(OT6 0x0402→0x0302 Headphones, IT1 0x0402→0x0201 Microphone)이다. 길이가 바뀌지 않는 수정이라 위험이 작다.

**C 결과 1 (2026-10-22 15:06, v0.2.0, `rewrite=terminal`, 로그 `dumps/usbhslog/usbhslog_20261022_150559.txt`):** **H2 확정.** OT6/IT1의 wTerminalType만 바꾸자 스위치가 이어폰을 "USB 스피커"로 표시하고 볼륨 UI도 동작했다. 하지만 소리는 나지 않았고, YouTube 홈브루는 오디오 디코딩 실패를 표시했다.
- 드라이버의 순서: FU5 GET_CUR/MAX/MIN 볼륨, GET_CUR 음소거, SET_CUR 볼륨 → IF2 SetInterface(alt 1) → OpenUsbEp(0x03, maxUrb 3, maxXfer 0x1e0) → CreateSmmuSpace(0x3000) → ShareReportRing(0x1000) → **SET_CUR 48000(엔드포인트)** → GetCurrentFrame → **BatchBufferAsync 3개(각 5 urb × 192바이트, unk1 0, unk2 = 현재 프레임+13, +18, +23)**. eoprobe에서 찾은 규약과 같다. 전송 보고는 공유 링으로 받아 IPC에는 나오지 않는다.
- 첫 3개 배치 뒤에 다음 배치가 오지 않았다. **원인은 v0.2.0 자신이었다.** 줄마다 fsync한 뒤 요청을 넘겨, 배치가 usb에 11~266 ms 늦게 도착했다. 시작 프레임이 이미 지나 전송이 실패했고, 드라이버의 스트림이 멈췄다. v0.2.1은 스트리밍 요청을 SD를 기다리지 않고 넘긴다.
- v0.2.0 로그에서 EP 객체 번호를 잘못 읽어 EP 요청(#2,#3,#6,#7,#8)이 루트 요청 이름으로 찍혔다. 도메인 대답에서 객체 ID는 7바이트 엔드포인트 디스크립터 바로 뒤(정렬 없음)에 온다. v0.2.1에서 고쳤다.

**C 결과 2 (2026-10-22 15:13, v0.2.1, `rewrite=terminal`, 로그 `dumps/usbhslog/usbhslog_20261022_151305.txt`): 성공.** 사용자 확인: 이어폰에서 홈 UI 소리와 홈브루(audout) 소리가 모두 난다. 스위치가 이어폰을 USB 스피커로 표시하고 볼륨 UI도 동작한다.
- 드라이버의 스트림: BatchBufferAsync(5 urb × 192바이트, unk2 = 프레임, 5씩 증가)가 **5초마다 정확히 1000개(= 5 ms마다 1개, 48 kHz 16비트 스테레오)**, IPC 실패 0, 약 2분 동안 이어졌다. 뽑을 때(15:15:29) 0x25A8C(장치 사라짐)로 정상 정리됐다.
- 15:13:36~38에 앨범 애플릿(홈브루) 실행과 겹쳐 GetCurrentFrame이 5번 불렸고, 그 5초 구간의 배치가 961개였다(약 0.2초 분량 빠짐). 드라이버가 스트림을 다시 맞춘 흔적이다. 이 순간 소리가 잠깐 끊겼는지는 사용자 확인이 필요하다.
- 디스크립터 고쳐 쓰기는 꽂을 때 3번(IF0/IF1/IF2용 GET_DESCRIPTOR) 일어났고 모두 다시 읽어 확인됐다.
- **결론: 접근 C가 목표를 달성했다.** 고칠 것은 출력/입력 단자 종류 3바이트뿐이었다(H2). 남은 확인: 리모컨 버튼(IF3/IF4는 hid가 잡음), 마이크, 슬립/깨우기, 다시 꽂기, TV 모드. 평소 사용에는 기록을 줄인 설정이 필요하다(`read_ctrl_data=0`, `sync=0`). `read_ctrl_data=1`이면 볼륨을 바꿀 때마다 audio에 디버거로 붙는다.

**FM 라디오와 리모컨 (2026-10-22, eofm 0.1.0~0.2.2, usbhs-logmitm 0.2.2~0.3.1, 로그 `dumps/eofmlog/`, `dumps/crash/`):**
- [switch/eofm](../switch/eofm/README.md): 이어폰 FM 칩을 벤더 요청(IF0 경유)으로 조작하는 한글 NRO. 실기기에서 방송 수신, 채널 찾기(14개), ZL/ZR 이동, 즐겨찾기가 동작한다. 칩의 성질(켜는 순서, QUERY가 마지막 결과를 되풀이함, 신호 세기는 QUERY에만 있음, 탐색은 방송 위에서 시작하면 바로 멈춤)은 eofm README에 정리했다.
- **스피커 출력은 포기했다.** 라디오를 USB 마이크로 받아 본체 스피커로 내려면 이어폰 마이크(IF1)를 오디오 모듈과 나눠 써야 한다. 두 방법 모두 오디오 모듈을 중단시켰다.
  - audin + `AppendUacInBuffer`: 잘못된 핸들로 2003-0501
  - 오디오 모듈의 목록에서 이어폰 숨기기: 비운 칸을 잡으려다 Data Abort, 이어서 qlaunch도 중단

  기록과 남은 아이디어는 [switch/eofm/attic/README.md](../switch/eofm/attic/README.md)에 있다. **교훈:** 오디오 모듈은 usb:hs 목록 개수를 믿지 않는다. 목록을 건드리면 안 된다. hid는 걸러진 목록을 정상적으로 받아들였다.
- **리모컨 볼륨:** 0.3.0~0.3.1은 hid의 목록에서 IF3/IF4를 빼고 sysmodule이 IF3을 직접 읽었다. 그러자 **오디오 모듈이 이어폰을 출력으로 등록하지 않았다.** 디스크립터를 읽은 뒤 IF1/IF2를 다시 잡지 않았다(17:28 로그). 같은 설정이면서 hid를 가로채지 않은 16:40 부팅은 정상이었다. 스위치 오디오는 헤드셋의 버튼 인터페이스를 hid가 맡고 있어야 헤드셋으로 받아들이는 것으로 보인다. 0.3.2는 hid가 IF3을 그대로 잡게 두고, hid의 PostBufferAsync 버퍼 주소와 GetXferReport를 보고 그 리포트를 hid 메모리에서 읽는다(실험, 기본 꺼짐).

**이어폰 출력이 다시 안 되던 문제 (2026-10-22 17:36, usbhs-logmitm 0.3.3에서 수정):** "평소 사용"으로 바꾼 `read_ctrl_data=0`, `sync=0` 부팅에서는 처음 꽂을 때 audio가 고친 디스크립터를 읽고도 이어폰을 출력으로 등록하지 않았다(16:40, 17:07, 17:17, 17:28, 17:36). 진단 설정이던 15:06/15:13만 처음부터 됐다. 고쳐 쓰는 코드는 같았다. 원인은 디버거 방식으로 쓴 바이트가 CPU 캐시에만 남고 audio가 DMA 데이터를 읽기 전에 캐시를 무효화하는 것으로 본다(진단 설정의 추가 읽기와 SD 지연이 우연히 메모리로 내려 보냈다). 0.3.3은 BindClientProcess로 지나가는 audio의 프로세스 핸들을 보관하고, 쓴 직후 `svcFlushProcessDataCache`를 호출한다. 기본 설정도 진단값으로 되돌렸다. hid 가로채기 탓으로 본 앞의 판단(0.3.2)은 틀렸다.

**남은 결정:** 게임 소리를 이어폰으로 들으려면 B(우리 sysmodule이 최종 믹스를 받아 직접 전송) 또는 C(닌텐도 드라이버가 받아들이게 MITM) 중 하나가 필요하다. libnx 4.12에는 B에 필요한 `audrec`(FinalOutputRecorder) API가 있다. 다만 게임 소리까지 녹음되는지, 권한·지연·스피커 음소거 상호작용은 실기기 확인이 필요하다.

---

## 1. 이전 답변 검증 결과

| 주장 | 판정 | 근거 |
|---|---|---|
| EO-IC100은 UAC1 디지털 기기, `04e8:a054`, 제조사 문자열 `bestechnic` | **확인** | 공개 `lsusb -v` 덤프 ([linux-hardware.org probe 7bc1964f22](https://linux-hardware.org/?probe=7bc1964f22&log=lsusb)) → [`fixtures/`](../fixtures/)로 바이트 단위 복원 (305바이트 = wTotalLength 일치) |
| 인터페이스 5개: IF0 AC / IF1 마이크(EP 0x83 IN) / IF2 재생(EP 0x03 OUT, alt1 16bit 44.1·48·96k, alt2 24bit) / IF3·IF4 HID | **확인** | 같은 덤프 |
| full-speed | **확인(실측)** | PC 덤퍼: full-speed (12 Mbps). 스위치 경로 문자열도 `FsDevice…` |
| PID 변형 존재 | **확인** | [kjy00302/eo-ic100_radio](https://github.com/kjy00302/eo-ic100_radio): `a054`, `a059`, `a05b` |
| FM 라디오 내장 | **확인** | 같은 저장소: vendor 컨트롤 요청(bRequest 0xA1/0xA2/0xA3) + IF4 인터럽트 EP(0x85)로 알림 |
| audio sysmodule이 usb:hs를 직접 씀 (5.1.0+) | **확인** | [switchbrew USB services](https://switchbrew.org/wiki/USB_services): "used by hid, bsdsockets, and [5.1.0+] audio" |
| audio sysmodule의 isoc 사용 | 부분 확인 | switchbrew: audio sysmodule은 `OpenUsbEp`에 maxUrbCount 3 또는 6을 넘김 |
| "UAC1만 / 16bit / 48 kHz" | 부분 확인 | 닌텐도 공식: 스위치 2 USB 스피커는 48 kHz 지원이 조건. 스위치 1의 정확한 조건은 미확인 |
| 닌텐도 파서가 "IF1=재생"을 가정해 실패 | **미확인 (가설 H1)** | 아래 §4 |
| "꽂으면 스위치가 멈춘다" (Reddit) | 미확인 | 출처 미확인 |
| switch-frame-tap / SysDVR-UVC가 가장 유사한 선례 | **부정확** | 둘 다 실재하지만 스위치가 USB *디바이스*로 PC에 영상을 보내는 구조(usb:ds)다. USB 오디오 *호스트* 구동의 선례가 아니다 |
| sys-con = usb:hs MITM 예 | **부정확** | sys-con은 usb:hs *클라이언트*(인터페이스를 직접 acquire)이다. MITM이 아니다 |
| libusbhsfs = NRO에서 usb:hs를 쓰는 선례 | 확인 | libnx 공식 예제 `examples/switch/usb/usbhs`도 같은 패턴 |

이번에 새로 확인된 사실(설계에 직접 영향):

- `usbHsEpBatchBufferAsync(ep, buf, urbs[], urbCount, id, unk1, unk2, &xferId)`의 **`unk1`/`unk2` 의미는 문서화되어 있지 않다.** PostBufferAsync는 내부적으로 0/0을 넘긴다(switchbrew). isoc 스케줄링(정책·시작 프레임)일 가능성이 높아서, 프로토타입은 8가지 해석을 차례로 시험하게 만들었다.
- [7.0.0+]에서 usb:hs 필터 구조체는 고유해야 하고 Flags가 0이면 안 된다. hid sysmodule은 `bInterfaceClass=3` 필터로 **모든 HID 인터페이스**를 감시한다. EO-IC100의 IF3/IF4가 hid에 잡혀 있을 수 있다.
- 다른 프로세스가 acquire한 인터페이스는 우리가 acquire할 수 없다. libnx 예제에 "Only devices which are not used by sysmodules are usable"이라고 적혀 있다. 그래서 **audio sysmodule이 IF0~2를 쥐고 있으면 접근 A는 그대로는 불가능**하다. 이 경우를 위해 race-acquire(재연결 순간 먼저 잡기)와 ResetDevice(소프트웨어 재연결)를 넣었다.

---

## 2. 이번에 구현한 것

| 단계 | 도구 | 하는 일 |
|---|---|---|
| 1 | [`pc/dump_usb_descriptors.ps1`](../pc/dump_usb_descriptors.ps1) | Windows에서 PnP 상태(오류 코드 포함)를 확인하고, 상위 허브 IOCTL로 디스크립터를 덤프한다(드라이버 교체 없음). `-Watch 1`로 플러그 유격(연결/해제 반복)을 감시한다 |
| 1~3 | [`pc/uacdesc.py`](../pc/uacdesc.py) | `.bin`/`.hex` 디스크립터 해석 → lsusb식 출력, 가설 플래그 H1~H7, 두 장치 `diff`, 스위치 덤프(`usbhs`) 해석 |
| 2·3 | [`switch/eoprobe`](../switch/eoprobe) (NRO) | 시스템 오디오 상태(auddev/audctl) + USB-C 역할(usb:pm), usb:hs 인터페이스 점유 상태, 디스크립터·HID 리포트 덤프, race-acquire / ResetDevice, **접근 A: isoc 톤 재생**(8가지 BatchBufferAsync 전략 스윕, 타이밍 측정), HID 버튼 모니터 |
| 테스트 | `pc/tests`, `switch/eoprobe/tests` | Python 16개, C 38개(ASan/UBSan + 변조 입력 2000개) — 모두 통과. NRO는 devkitA64(GCC 15.2, libnx 4.12)로 경고 0으로 빌드됨 |

---

## 3. 실행 순서 (사용자 작업)

### Step 1 — 이어폰 자체 확인 (PC, 5분)

```powershell
powershell -ExecutionPolicy Bypass -File pc\dump_usb_descriptors.ps1
```

| 결과 | 판정 |
|---|---|
| `04e8:a054`(또는 a059/a05b)가 Status OK로 보이고 `config descriptor: 305 bytes` | 정상. Step 2로 |
| 장치가 안 보이거나 ConfigManagerErrorCode가 0이 아님 | 이어폰/플러그 문제. `-Watch 1`을 켜고 플러그를 흔들어 DISCONNECTED가 찍히는지 본다. 이 경우 스위치 드라이버로 해결할 수 없다 |
| 305바이트가 아니거나 analyzer 출력이 fixture와 다름 | 펌웨어 변형. `dumps\*_config.bin`을 보관해 둔다(이후 모든 분석의 기준) |

### Step 2 — 스위치 진단 (eoprobe, 5분)

1. `switch/eoprobe/eoprobe.nro`를 SD의 `/switch/eoprobe/eoprobe.nro`로 복사한다.
2. 이어폰을 **꽂은 상태로** hbmenu에서 실행한다(앨범 애플릿 모드여도 된다). 실행하면 자동으로 스캔한다.
3. 화면 해석:

| 화면 | 의미 | 다음 행동 |
|---|---|---|
| `No USB device is visible to usb:hs` | 드라이버보다 아래 단계(USB-C 역할/전원/열거) 실패 | `usb:pm` 줄(power role, data role) 확인. 독의 USB-A 포트 + C→A 변환젠더로 재시도. 소프트웨어 드라이버로는 해결 불가할 가능성이 높다 |
| IF0~2가 `held by another process` 또는 `not listed` | audio sysmodule이 쥐고 있다(인식은 했지만 실패 중이라는 뜻) | **R**(race-acquire). 소프트웨어 리셋이 안 되면 이어폰을 뽑았다 꽂는다 → IF2가 `ours`가 되면 Step 3 |
| IF0~2가 `available`/`ours` | audio sysmodule이 이 장치를 아예 잡지 않는다 | Step 3 |
| `flags: [H1 CAPTURE_FIRST] [H2 ...]` | 디스크립터 기반 가설 플래그 | 결과 보고에 포함 |
| `auddev active output` / `audctl active` | 시스템이 어떤 출력을 쓰고 있다고 보는지 | 이어폰을 꽂았을 때와 뺐을 때를 비교하면 좋다 |

### Step 3 — 접근 A 프로토타입: 직접 소리 내기 (10분)

> 처음에는 이어폰을 귀에서 조금 떼고 시작한다. 기본값은 −30 dBFS 톤에 장치 볼륨 (최대 −20 dB)이다.

1. **X**(톤 재생, B로 정지, 최대 60초): v0.2.0부터 기본 전략 0번(`0/frame`)이 실기기에서 확인된 방식이다. **ZL**을 켜면 좌→우→양쪽 순서로 채널 매핑을 확인할 수 있다.
2. **Y**(스윕): 8가지 전략을 1.5초씩 돌린다. 다른 펌웨어에서 호출 규약이 바뀌었는지 확인하는 용도다. 화면 요약을 읽는 법:
   - `PACED 192.0 kB/s` → isoc 전송이 1 ms 프레임 속도에 맞춰 진행됐다(**성공 신호**)
   - `TOO FAST` → 전송이 isoc 타이밍으로 처리되지 않았다(인자 해석이 틀림)
   - `submit rejected` / `no completions` / `all transfers failed` / `SLOW` → 해당 인자 조합은 거부되거나 일부만 성공
3. 결과가 다르면 ←/→로 전략을 골라 X로 다시 들어 본다.
4. **L**(HID 모니터): 리모컨 버튼을 눌러 IF3 리포트(3바이트)가 들어오는지 본다. 인터럽트 전송이 되는지 확인하는 용도로, 접근 B/C에서 볼륨 버튼에 쓴다.

> 스윕은 검증되지 않은 인자로 usb sysmodule을 호출한다. 최악의 경우 시스템 크래시(재부팅)가 날 수 있으니 게임을 종료하고 실행한다. 각 전략 직전에 로그를 SD에 fsync하므로, 크래시가 나도 어느 전략에서 멈췄는지 남는다.

### Step 4 — 결과 회신

- SD의 `/switch/eoprobe/` 폴더 전체: `logs/eoprobe_<날짜_시각>.txt`(v0.2.2부터 실행마다 새 파일), `*_config.bin`, `usbhs_query_*.bin`, `*_hid_if*.bin`
- PC의 `dumps\` 폴더
- 전략별로 소리가 났는지(안 났음 / 잡음 / 깨끗한 톤 / 좌우 정상), 그리고 펌웨어와 Atmosphère 버전
- 가능하면 **스위치에서 정상 동작하는 다른 USB-C 이어폰**을 같은 도구로 덤프한 결과(가장 강력한 비교 자료):
  ```
  python pc곞sc.py diff dumps\<정상기기>_config.bin dumps\04e8_a054_config.bin
  ```

---

## 4. 왜 닌텐도 드라이버가 실패하는가 — 가설 H1~H7

공개 디스크립터에서 바로 보이는 특이점이다. `uacdesc.py`와 NRO가 같은 기준으로 플래그를 띄운다.

| ID | 관찰 (EO-IC100) | 흔한 동작 기기 | 실패 시나리오 |
|---|---|---|---|
| H1 | 첫 AS 인터페이스(IF1)가 **마이크**, 재생은 IF2 | 재생 AS가 먼저 | 첫 AS를 재생으로 가정하는 파서가 IN EP에 재생 데이터를 넣음 |
| H2 | 재생 경로의 출력 터미널 타입이 **0x0402 Headset**(양방향 타입) | 0x0301 Speaker / 0x0302 Headphones | 출력 장치를 0x03xx 타입으로만 찾으면 "출력 없음" |
| H3 | 재생 alt1이 **44.1/48/96 kHz 다중 레이트** → SET_CUR 필요 | 48 kHz 단일 | 호스트가 SET_CUR를 안 보내면 기기가 다른 레이트로 동작 |
| H4 | EP 동기 타입 **Synchronous** | Adaptive | 지원 타입 화이트리스트 |
| H5 | EP 번호 3을 IN(0x83)/OUT(0x03) **공유** | 서로 다른 번호 | 번호로만 EP를 관리하는 구현 |
| H6 | 마이크가 **2채널** | 모노 | 캡처 경로 검증 실패가 장치 전체 거부로 이어짐 |
| H7 | HID 인터페이스 2개(리모컨, FM 알림) 추가. **IF4 리포트 디스크립터가 규격 위반**(Logical Max −1 < Min 0, 예약된 Usage Page 0xFEFF) | 0~1개, 정상 디스크립터 | hid sysmodule 선점(실측으로 확인), 또는 잘못된 HID 때문에 장치 전체를 거부. Windows도 IF4에서 Code 10을 낸다 |

실측 관찰: audio sysmodule은 **IF1·IF2(AS)만 잡고 IF0(AC)는 놓아둔 채** 출력 장치를 만들지 않았다. "AS 인터페이스를 acquire한 뒤의 판정(포맷/터미널/레이트 검사)에서 포기한다"는 쪽과 맞는 정황이다. H1~H4, H6이 유력하다.

wMaxPacketSize가 384(96 kHz 기준)인 것도 정보 수준으로 표시한다. wMaxPacketSize로 패킷 크기를 정하는 드라이버라면 2배속으로 재생된다.

**확정 방법(우선순위 순):**
1. **비교 덤프**: 스위치에서 동작하는 USB-C 이어폰과 `uacdesc.py diff` → 차이 나는 항목만 남기기
2. **C-0 로깅 MITM**(§5): audio sysmodule이 실제로 어떤 요청을 보내고 어디서 멈추는지 직접 본다
3. (선택) USB 가젯 A/B 테스트: Raspberry Pi Zero 등의 Linux `g_audio`/configfs로 EO-IC100과 같은 레이아웃을 흉내 내고, H1~H7을 하나씩 고쳐 스위치에 꽂아 본다

---

## 5. 다음 구현 단계 (Step 3 결과에 따라)

```
Step 3 결과                       (2026-10-20: 첫 번째 분기로 확정)
├─ PACED + 소리 들림 ────────────► A 성공. "usb:hs로 이 이어폰 구동 가능" 확정
│                                  → C-0(로깅 MITM)로 H1~H7 확정 → C 또는 B
├─ PACED인데 무음/잡음 ───────────► 레이트/볼륨/뮤트/포맷 문제. 로그의 SET_CUR/GET_CUR, FU 값 확인 후 수정
├─ 모든 전략 TOO FAST/거부 ───────► unk1/unk2 의미를 모름 → C-0로 *정상 USB 이어폰* 재생 시
│                                  audio sysmodule의 BatchBufferAsync 인자를 기록해 해답 확보
└─ IF2를 끝내 못 잡음 ────────────► 접근 A 불가 → C-0/C(MITM) 경로로 직행
```

### C-0. usb:hs 로깅 MITM — [`switch/usbhs-logmitm`](../switch/usbhs-logmitm/README.md) v0.1.0 구현됨, 실기기 미검증
- **audio 프로세스의 `usb:hs` 세션**(그리고 1단계 검증용으로 hbmenu 홈브루)을 Atmosphère MITM으로 가로챈다. libstratosphere 없이 libnx C로, eoaudio v0.4.x audout MITM에서 확인된 sm 절차를 그대로 쓴다(전용 `sm:` 세션 + RegisterClient, InstallMitm 뒤 반드시 ClearFutureMitm, 질문은 전용 스레드·설치는 메인 스레드, PID 태그 `0xFFFE<<48`, 포인터 버퍼 재지정, 크래시 때 UninstallMitm 먼저). 요청은 바꾸지 않고 그대로 전달한다. 프로그램 ID `420000000E01C0C0`.
- 기록 내용:
  - sm의 ShouldMitm 질문 전부(어떤 프로세스가 언제 usb:hs를 여는지)
  - root: Query*Interfaces 필터와 결과(인터페이스별 번호·alt·클래스·엔드포인트·VID:PID), CreateInterfaceAvailableEvent, AcquireUsbIf
  - IClientIfSession: SetInterface/GetAlternateInterface 결과, **CtrlXferAsync setup packet(해석 포함) + GetCtrlXferReport 결과**, OpenUsbEp
  - **컨트롤 전송 데이터**: audio가 받은 디스크립터 바이트와 보낸 SET_CUR 값. 버퍼는 audio의 힙에 있어 svcMapProcessMemory가 거부되므로(eoaudio v0.4.7의 0xD401), dmnt 치트처럼 `svcDebugActiveProcess` → `svcReadDebugProcessMemory` → 닫기로 읽는다(npdm `force_debug`). 이 방식이 되면 C의 디스크립터 재작성에 같은 경로의 `svcWriteDebugProcessMemory`를 쓸 수 있다.
  - IClientEpSession: **BatchBufferAsync의 urbCount/unk1/unk2/urb 크기**, XferReport(초당 수백 번이라 세션마다 처음 40개만 자세히, 이후 5초마다 개수)
  - 줄마다 fsync 뒤에 요청을 넘긴다(`sync=1`). 도메인으로 바뀐 세션도 객체 ID를 따라가며 기록한다(통과만 시키므로 해석이 틀려도 전달에는 영향 없음).
- **부팅 타이밍 문제와 해결:** audio는 부팅 초기에 usb:hs를 한 번 열고 계속 쓰는 것으로 보인다. boot2.flag로 뜨는 sysmodule은 그 뒤에 시작하므로 그냥 설치해서는 audio 세션을 볼 수 없다. 그래서 `atmosphere/contents/420000000E01C0C0/mitm.lst`(`usb:hs` 한 줄)를 쓴다. boot2가 이 파일을 보고 sm에 future mitm을 선언하면 audio·hid의 GetServiceHandle(usb:hs)이 이 도구가 설치와 ClearFutureMitm을 마칠 때까지 기다린다. 그래서 main은 SD 마운트 직후, 시계·로그 파일보다 먼저 MITM을 올린다(그 사이 줄은 RAM 링에 보관). 설치가 실패해도 ClearFutureMitm은 보낸다.
- **시험은 2단계**(README): 1단계는 mitm.lst 없이 설치해 eoprobe(홈브루) 트래픽으로 전달 경로를 검증한다(문제가 생겨도 eoprobe만 영향). 2단계에서 mitm.lst를 넣고 재부팅해 게임 실행 중 이어폰을 꽂는다. 부팅이 멈추면 Hekate UMS로 폴더를 지워 복구한다.
- **eoaudio와 충돌:** eoaudio는 꽂는 순간 5개 인터페이스를 모두 선점하므로, C-0 시험 동안 `420000000E01C100/flags/boot2.flag`를 지워 꺼 둔다.
- 얻는 것: ① 닌텐도 드라이버가 EO-IC100에서 멈추는 정확한 지점(H1~H7 확정) ② 그 지점이 IPC 대답(인터페이스 정보, 전송 결과)인지 audio 메모리 속 데이터(디스크립터)인지 → C의 구현 방식 결정
- 알려진 위험: 가로채는 동안 이 프로세스가 죽으면 audio 세션도 끊긴다(재부팅으로 복구). mitm.lst가 있는데 프로세스가 못 뜨면 hid·audio가 기다려 부팅이 멈춘다. Ultrahand/ovl-sysmodules로 끄지 못하게 `toolbox.json`을 넣지 않았다.

### C. 디스크립터 재작성 MITM (정석 해결) — usbhs-logmitm v0.2.0 첫 빌드, 실기기 미검증
- **v0.2.0 (2026-10-22):** C-0과 같은 프로그램에 `rewrite=` 규칙을 넣었다. audio가 EO-IC100의 GET_DESCRIPTOR(CONFIG)을 받으면, GetCtrlXferReport 대답을 돌려주기 전에 audio 버퍼를 디버거 방식으로 읽고, 규칙을 적용해 `svcWriteDebugProcessMemory`로 다시 쓰고, 다시 읽어 확인한다. 규칙은 모두 길이를 바꾸지 않는다(`source/rewrite.c`, 실기기 디스크립터로 호스트 테스트). 기본은 `terminal`(H2: OT6 0x0402→0x0302, IT1 0x0402→0x0201)이다. `single_rate`(H3), `alt1_only`(24/32비트 alt 숨김), `mps48`(wMaxPacketSize 384→192)을 하나씩 더할 수 있다. 시험 방법은 README "3단계"에 있다.
- 남은 위험: 쓴 바이트가 audio에 보이는지(캐시). 커널이 쓰고 다시 읽은 값은 로그에 남지만, audio가 이후 캐시 무효화로 원래 DMA 데이터를 다시 볼 가능성은 로그의 드라이버 반응으로만 판단할 수 있다.
- C-0 위에 변환 계층을 둔다. GET_DESCRIPTOR(CONFIG) 응답과 Query/Acquire 결과의 인터페이스 정보를 "닌텐도가 받아들이는 형태"로 바꾼다. 예: H1이면 AS 인터페이스 순서 교체 + 번호 재매핑(SetInterface/wIndex 역변환), H3이면 단일 48 kHz alt로 보이게 하고 SET_CUR를 MITM이 대신 보냄.
- 변환 규칙은 먼저 `uacdesc.py`에서 구현·검증한 뒤 C로 옮긴다(파서와 fixture가 이미 있음).
- **C가 가능한 이유(C-0 설계에서 확인한 것):** MITM이 바꿀 수 있는 곳은 세 군데다. ① IPC 대답(Query/Acquire의 인터페이스 정보, SetInterface 결과, 전송 보고): MITM 안에서 바로 바꿀 수 있다. ② audio가 보내는 요청 인자(CtrlXferAsync의 setup packet, SetInterface 번호, OpenUsbEp 인자): IPC 데이터라서 전달 전에 바꿀 수 있다(인터페이스 번호 재매핑 등). ③ 컨트롤 전송 데이터(디스크립터 바이트): usb sysmodule이 audio 메모리에 직접 쓰므로 IPC로는 못 바꾼다. GetCtrlXferReport를 돌려주기 전에 `svcWriteDebugProcessMemory`로 덮어쓰고 보고의 transferredSize를 맞춘다. C-0 v0.1의 메모리 읽기가 실기기에서 되면 이 경로도 열린 것이다. 안 되면 ③이 필요한 경우(디스크립터 내용이 원인)만 막히고 ①②로 풀리는 원인은 여전히 C로 해결할 수 있다.
- 성공하면 시스템 볼륨, 출력 전환, 리모컨까지 네이티브로 동작한다.

### B. 우회 sysmodule (빠른 우회) — [`switch/eoaudio`](../switch/eoaudio) v0.2.0 구현됨, 실기기 미검증
실기기에서 확인된 부품을 백그라운드 sysmodule 하나로 묶었다(libnx C, libstratosphere 없음). ✅는 eoprobe로 확인된 것, 🛠️는 구현했지만 실기기 확인 전, ❓는 실기기 로그로 처음 확인하는 것이다. 설치, 제거, 시험 방법은 [switch/eoaudio/README.md](../switch/eoaudio/README.md)에 있다. v0.2.0부터 기본 모드는 `stream`(모든 소리를 이어폰으로)이다. `capture`와 `tone`은 진단용으로 남겼다.
1. ✅🛠️ **장치 확보**: InterfaceAvailableEvent(VID 04E8) + USB 이벤트 뒤 10초간 2 ms 간격 QueryAvailable. 04e8이면서 PID가 `pids=`(a054/a059/a05b)에 있는 장치만, 확보하기 전에 확인하고 잡는다. 스트리밍 → AC → HID 순서로 전부 acquire하고, 재열거로 세션이 사라지면 새 인스턴스를 다시 잡는다. `settle_ms`(1초) 동안 변화가 없으면 설정한다. 재생 인터페이스를 시스템이 먼저 잡았으면 ResetDevice로 다시 연결시켜 경쟁한다(최대 2회). 다른 USB 장치는 2초마다 QueryAll 목록으로만 비교해 VID:PID와 판단("left alone")을 로그에 남긴다.
2. ✅🛠️ **설정**: 디스크립터에서 48 kHz/16/2 재생 alt를 찾아(IF2 alt1, EP 0x03) SET_INTERFACE, SET_CUR 48000, FU5 볼륨/음소거.
3. ✅🛠️ **전송**: eoprobe `isoc.c`를 연속 스트리밍으로 바꿨다. `BatchBufferAsync(unk1=0, unk2=절대 프레임)`, 4 urb × 4 배치, 0xD28C면 재동기화, 보고는 제출 id와 정확히 맞춘다(eoprobe 로그에서 urb 4개 모두 같은 id임을 확인). 오디오는 제출 직전에 채운다. 스트리밍·녹음 스레드는 SD에 쓰지 않는다(로그는 RAM 링 → 우선순위가 낮은 스레드).
4. ❓🛠️ **소리 원천**: `audrec:u` FinalOutputRecorder(48 kHz, 2 ch 요청), aruid 0 → 자기 pid → 게임 pid 순으로 연다. 5 ms 버퍼 8개를 돌리고, 버퍼마다 표식 값을 미리 채워 덜 채워진 버퍼를 센다. 6채널이면 다운믹스한다. 10초(stream에서는 3초) 동안 버퍼가 안 오면 다시 연다. **1단계(`mode=capture`)**: 이어폰으로 소리를 내지 않고 초당 RMS/피크, 버퍼 간격, partial, GetFinalOutputRecorderBufferEndTime, 실행 중인 게임 ID, 콘솔 볼륨 변화를 기록한다.
5. 🛠️ **클럭 차이**: 소비 쪽 지터 버퍼(목표 20 ms). 평균 수위가 목표에서 버퍼 1/4 이상 벗어나면 4 ms 블록 하나를 193/191 → 192 프레임으로 선형 보간해 한 샘플을 빼거나 넣는다(최대 100 ms에 한 번 = 200 ppm 이상, 크게 벗어나면 블록마다). 호스트 시뮬레이션(`tests/test_pcm.c`): ±200/±1000 ppm, 0~4 ms 지터, 21 ms 덩어리 입력에서 끊김 0. 입력이 60 ms 멈추면 한 번 끊긴 뒤 다시 채워서 재생하고, 125 ms가 한꺼번에 오면 목표량으로 잘라낸다.
6. ✅🛠️ **리모컨과 볼륨**: IF3 인터럽트 리포트 → Vol± `volume_step_db`(3 dB, 길게 누르면 반복), `volume_min_db`~`volume_max_db` 범위. FU5로 바꾸고, FU5가 없거나 실패하면 디지털 게인으로 대신한다. 마지막 볼륨은 `state.ini`에 저장해 재연결·재부팅 뒤에도 쓴다. 가운데 버튼(`01 04 00` 예상, 미확인) = 이어폰 소리 끄기/켜기(`play_button=mute|none`, 누를 때만). 가상 컨트롤러(hiddbg)로 HOME/+를 누르는 기능은 플레이어 배정을 바꿀 위험이 있어 넣지 않았다.
7. ❓🛠️ **스피커/TV 끄기** (`speaker_mute=auto`, `source/policy.c`, 호스트 테스트 12개 시나리오): 이어폰 스트림이 1초 이상 정상이면 활성 출력(스피커 또는 TV)을 끈다. 확인되지 않은 방법은 녹음 레벨이 −50 dBFS보다 클 때 적용하고, 녹음이 무음(−75 dBFS 아래)이 되면 되돌린 뒤 녹음이 돌아오는지 본다. 두 번 돌아오면 그 방법을 버리고(SetTargetMute → SetTargetVolume(min) → 그대로 둠), 결과를 펌웨어별로 `state.ini`에 기억한다. capture 모드 `mute test`의 "capture kept" 결과도 그대로 쓴다. 이어폰을 빼면 바로, 크래시 때는 예외 처리기가, pm 강제 종료 뒤에는 다음 시작 때 `speaker_restore.txt`로 되돌린다. 음소거로 끈 동안은 콘솔 볼륨 변화를 이어폰 볼륨 단계로 따라간다(`volume_follow`).
8. 🛠️ **물러나기**: audctl 활성 출력이 3.5 mm, Bluetooth, USB 출력 장치(닌텐도가 인식한 다른 USB 헤드셋)가 되면 이어폰 스트림을 멈추고 아무것도 끄지 않는다. 스피커/TV로 돌아오면 다시 재생한다.
9. 🛠️ **로그**: 실행마다 새 파일. USB 장치 연결/해제(VID:PID, 판단), 확보 경쟁, 재열거, 상태 전환, 버튼과 동작, 볼륨/음소거, 스피커 끄기/되돌리기, 활성 출력 변화, 10초마다 `health:` 한 줄(녹음 버퍼·레벨, USB 배치·실패·재동기화, 지터 버퍼 수위·드리프트 보정·underrun). `log_level=debug`면 1초 상세 줄과 리모컨 원본 리포트를 더한다.
- **안전장치**: 예외 처리기(크래시 → `logs/<실행>_crash.txt`, 스피커 복구, eoaudio만 종료), boot guard(60초 안에 끝난 실행 3번 연속이면 스스로 꺼짐), `mode=off`, `boot2.flag` 삭제, SD 없음·시계 없음에서도 abort하지 않음.
- **실기기 결과에 따른 분기**: 소리가 이어폰으로 나옴 → 끊김/지연/스피커 끄기 판정 로그로 조정. 소리가 안 남 → `mode=tone`(전송 확인) → `mode=capture`(녹음 확인). 녹음기가 안 열리거나 무음이면 B는 보류하고 C-0/C로 간다(`grc:d`는 두 번 초기화하면 시스템이 assert한다고 알려져 있어 쓰지 않았다).
- 한계: 녹화를 막는 게임은 무음일 수 있음, 지연(녹음기 내부 + 약 41 ms 예상), 시스템 볼륨 UI는 음소거 방식일 때만 따라감, 시스템 업데이트에 취약.

---

## 6. 안전 수칙
- 청력: 첫 재생은 귀에서 떼고 한다. 레벨은 ↑/↓로 조절(−48~−12 dBFS). 장치 볼륨은 앱이 (최대 −20 dB)로 설정한다.
- 스윕은 usb sysmodule을 크래시시킬 수 있다. 게임을 종료하고 실행하며, 크래시가 나면 재부팅한다.
- race-acquire와 ResetDevice는 앱이 실행되는 동안만 sysmodule에게서 인터페이스를 가져온다. 앱을 종료하면 반환된다.
- PC 덤프의 `*_strings.txt`에서 시리얼은 기본으로 가려진다(`-KeepSerial`로 해제).
