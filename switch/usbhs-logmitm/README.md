# usbhs-logmitm — 접근 C-0/C: 오디오 드라이버의 usb:hs 기록과 디스크립터 고쳐 쓰기

스위치 자체 오디오 드라이버(audio sysmodule, `0100000000000014`)가 EO-IC100(`04e8:a05b`)을 USB 헤드셋으로 받아들이지 않는 지점을 찾기 위한 기록 도구입니다. Atmosphère MITM으로 audio와 `usb:hs` 사이에 끼어들어, **모든 요청을 바꾸지 않고 그대로 전달**하면서 SD 카드에 기록합니다.

- 프로그램 ID: `420000000E01C0C0` (sysmodule, libnx C)
- 버전: 0.3.2 (2026-10-22). v0.1.0의 C-0 기록과 v0.2.0의 `terminal` 고쳐 쓰기가 실기기에서 동작했다(결과: docs/PLAN.md §0).
  - v0.3.2: 리모컨 볼륨을 **엿보기 방식(실험, 기본 꺼짐)**으로 바꿨다. hid가 리모컨(IF3)을 그대로 잡고, 이 sysmodule은 hid가 받은 리포트를 hid 메모리에서 읽어 볼륨을 바꾼다. 켜려면 `programs=audio,hid`, `remote=1`. 0.3.0~0.3.1처럼 hid에게서 리모컨을 숨기면 **오디오 모듈이 이어폰을 출력으로 받아들이지 않았다**(실기기, 17:17/17:28 부팅).
  - v0.3.1: 이어폰 리모컨 볼륨(첫 시도, 0.3.2에서 바꿈). hid가 받는 인터페이스 목록에서 리모컨(IF3/IF4)을 빼고, 이 sysmodule이 IF3을 직접 읽었다. Vol+/−는 FM 라디오가 켜져 있으면 라디오 볼륨, 아니면 스위치 출력 볼륨(audctl)을 한 칸씩 바꾼다(스위치 볼륨 막대는 뜨지 않음). `programs=audio,hid`, `remote=1`이 필요하다. 부팅할 때 남은 `rewrite_override.txt`를 지운다.
  - v0.3.0: 오디오 모듈에게서 이어폰을 숨기는 기능(eofm 스피커 출력용)을 넣었다가 0.3.1에서 껐다. 오디오 모듈이 목록 개수를 믿지 않고 비운 칸을 잡으려다 중단됐다(콘솔 오류). 리모컨은 잡자마자 놓는 버그가 있었다.
  - v0.2.2: `rewrite_override.txt`(앱이 이번 부팅 동안만 규칙을 바꾸는 파일)를 읽는다.
  - v0.2.1: 스트리밍 요청(isoc 전송, 프레임 번호)은 SD 기록을 기다리지 않고 바로 넘긴다. v0.2.0은 줄마다 SD에 쓰느라 audio의 isoc 전송이 시작 프레임(현재+13 ms)을 놓쳐 소리가 나지 않았다. 엔드포인트 객체 번호 해석도 고쳤다(로그에서 EP 요청이 루트 요청처럼 찍히던 문제).
  - v0.2.0: `rewrite=` 규칙으로 audio가 받는 EO-IC100 설정 디스크립터를 고쳐 쓴다. 기본값 `terminal`(H2).
  - v0.1.1: 로그 파일을 쓸 때마다 닫아 DBI MTP로 바로 복사 가능, sm 명령 번호 표시 수정, 슬립 뒤 상태 줄 반복 수정.
- 계획 문서: [docs/PLAN.md](../../docs/PLAN.md) §5 C-0

## 무엇을 기록하나

| 대상 | 내용 |
|---|---|
| sm의 질문 | usb:hs를 여는 모든 프로세스(audio, hid, bsdsockets, 홈브루)와 가로챘는지 여부 |
| usb:hs 루트 | 인터페이스 조회 필터와 결과(인터페이스 번호, alt, 클래스, 엔드포인트, VID:PID), 이벤트 생성, `AcquireUsbIf` |
| 인터페이스 세션 | `SetInterface`/`GetAlternateInterface` 결과, **컨트롤 전송**(setup packet 해석: GET_DESCRIPTOR, SET_INTERFACE, UAC SET_CUR/GET_MIN 등)과 그 결과, `OpenUsbEp` |
| 컨트롤 전송 데이터 | 오디오가 받은 디스크립터 바이트(한 줄씩 해석), 오디오가 보낸 SET_CUR 값. audio 프로세스 메모리를 디버거 방식으로 잠깐 읽습니다(`read_ctrl_data`) |
| 엔드포인트 세션 | isoc 전송(`BatchBufferAsync`의 urb 수·크기·시작 프레임), 전송 보고. 초당 수백 번 오므로 처음 40개만 자세히, 이후 5초마다 개수만 |

로그 줄은 SD에 쓴 뒤(fsync) 요청을 넘깁니다. 콘솔이 멈추거나 재부팅돼도 마지막 줄이 그 직전 단계입니다.

## 시험 전 준비 (필수)

1. **eoaudio(접근 B)를 꺼 둡니다.** eoaudio는 이어폰을 꽂는 순간 5개 인터페이스를 모두 먼저 잡습니다. 그러면 닌텐도 드라이버가 이어폰을 만져 보지도 못해 기록할 것이 없습니다.
   - 이어폰을 뽑은 상태에서 SD의 `atmosphere/contents/420000000E01C100/flags/boot2.flag`를 지웁니다(또는 이름을 `boot2.flag.off`로 바꿉니다). 재부팅하면 eoaudio가 시작되지 않습니다.
   - 나중에 eoaudio를 다시 쓰려면 이 파일을 되돌리고 재부팅합니다. eoaudio와 이 도구를 동시에 켜 두지 마세요.
2. 펌웨어와 Atmosphère는 지금 그대로(HOS 22.1.0) 둡니다.
3. **복구 방법을 미리 확인합니다.** 2단계는 부팅 중에 끼어듭니다. 문제가 생기면 부팅이 멈출 수 있습니다. 그때는 전원 버튼을 12초 눌러 끄고, Hekate → Tools → USB Tools → SD Card(UMS)로 PC에 연결해 `atmosphere/contents/420000000E01C0C0` 폴더를 지운 뒤 재부팅하면 원래대로 돌아옵니다.

## 설치

`usbhs-logmitm-v0.1.1-sd.zip`의 구성:

```
stage1/atmosphere/contents/420000000E01C0C0/exefs.nsp
stage1/atmosphere/contents/420000000E01C0C0/flags/boot2.flag
stage1/switch/usbhs-logmitm/config.ini
stage2/atmosphere/contents/420000000E01C0C0/mitm.lst
```

DBI MTP 또는 Hekate UMS(E:)로 **`stage1` 폴더 안의 내용**을 SD 루트에 복사합니다(`E:\atmosphere\...`, `E:\switch\...`가 되도록). 2단계 파일은 1단계 결과를 확인한 다음에 복사합니다.

> Ultrahand/ovl-sysmodules 목록에는 일부러 나오지 않게 했습니다(`toolbox.json` 없음). 가로채는 중에 이 프로세스를 끄면 다음에 usb:hs를 여는 프로그램이 생길 때 sm이 fatal을 냅니다(eoaudio v0.4.6에서 겪은 문제). 끄려면 아래 "끄기/제거"를 따르세요.

## 1단계: 안전 확인 (부팅 가로채기 없음)

목적: MITM이 sm과 제대로 대화하고 요청을 깨지지 않게 전달하는지 홈브루로 먼저 확인합니다. 이 단계에서는 audio가 이미 usb:hs에 연결된 뒤에 시작하므로 audio는 대개 잡히지 않습니다.

1. 1단계 파일을 복사하고 재부팅합니다.
2. 콘솔이 평소처럼 부팅되고 조이콘·소리가 정상인지 봅니다.
3. 게임을 끈 상태에서 이어폰을 꽂고 hbmenu에서 **eoprobe**를 실행합니다. eoprobe는 홈브루라서 가로챕니다(`programs=...,homebrew`).
   - 자동 스캔 결과가 평소처럼 나오는지 봅니다.
   - **X**(톤 재생)로 삐 소리가 나는지 듣습니다(이어폰을 귀에서 조금 떼고). 이 소리는 MITM을 거쳐 나갑니다. 몇 초 뒤 **B**로 멈추고 **+**로 종료합니다.
   - R(race)은 쓰지 않습니다. 줄마다 SD에 기록하느라 느려집니다.
4. SD의 `switch/usbhs-logmitm/logs/` 폴더를 보내 주세요.

| 결과 | 판정 |
|---|---|
| 부팅 정상, eoprobe 정상, 톤 들림, 로그에 `S1 ... CtrlXferAsync`, `BatchBufferAsync` 줄 | 전달 경로 정상 → 2단계로 |
| eoprobe가 이상하거나 콘솔이 멈춤 | 로그의 마지막 줄이 원인 단계. 2단계로 가지 말고 로그를 보내 주세요 |
| 로그에 `mitm: InstallMitm(usb:hs): 0x...` 오류 | 설치 실패. 로그를 보내 주세요 |

## 2단계: 부팅 때부터 audio 가로채기

`mitm.lst`가 있으면 Atmosphère boot2가 부팅 초기에 "usb:hs에 MITM이 곧 생긴다"고 sm에 알립니다. 그러면 audio·hid가 usb:hs를 열 때 이 도구가 뜰 때까지 기다리고, audio의 세션이 처음부터 이 도구를 거칩니다.

1. `stage2` 폴더 안의 내용을 SD 루트에 복사합니다(`atmosphere/contents/420000000E01C0C0/mitm.lst`).
2. 재부팅합니다. **부팅이 정상인지, 조이콘이 동작하는지, 콘솔 스피커 소리가 나는지** 확인합니다(hid와 audio가 이 도구를 기다렸다가 출발했다는 뜻).
3. 게임을 하나 실행합니다(소리가 나는 화면). 닌텐도 드라이버는 게임이 실행 중일 때만 이어폰을 잡습니다(§0 관측).
4. **이어폰을 꽂고** 10초 기다립니다. 꽂으면 장치가 약 3번 다시 열거됩니다.
5. 게임 소리가 어디서 나는지(스피커/이어폰/안 남) 메모합니다. 콘솔 볼륨을 한 번 올렸다 내립니다.
6. 이어폰을 뽑고 5초 기다린 뒤, 다시 꽂고 10초 기다립니다.
7. 게임을 종료합니다. 1분쯤 기다린 뒤(상태 줄이 1분마다 남음) 로그를 가져옵니다.

로그 보내실 것: `switch/usbhs-logmitm/logs/` 전체(크래시가 있었다면 `*_crash.txt` 포함)와 메모(소리가 어디서 났는지, 이상 증상).

### 로그 읽는 법

```
+00009.812 mitm: sm asks (cmd 65000) about usb:hs audio (pid 82, flags 0x0): INTERCEPT, reply OK
+00009.815 mitm: audio (pid 82) connected to usb:hs: session S1
14:02:31.204 S1 audio usb:hs #4 CreateInterfaceAvailableEvent: in [...]
14:02:35.877 S1 audio usb:hs #7 AcquireUsbIf: in [...]
     id 33 ... if 1 alt 0 eps 0 class 01/02/00
   new session S5 audio IF[id 33 if 1]
14:02:35.902 S5 audio IF[id 33 if 1] #5 CtrlXferAsync: IN  std    dev GET_DESCRIPTOR CONFIG idx 0 lang/if 0x0 len 345 ...
     IN data (345 bytes):
       @000 CONFIG total 345 ifs 5 ...
```

- `+sssss.mmm`는 부팅 후 초(시계를 읽기 전에 남긴 줄), `hh:mm:ss.mmm`는 시각입니다.
- `S<번호>`는 세션, `IF[...]`는 인터페이스 세션, `EP[0x03]`는 엔드포인트 세션입니다.
- `->` 줄은 usb sysmodule의 대답입니다. `OK`가 아니면 그 요청이 실패한 것입니다.
- `status:` 줄(1분마다)은 누적 개수입니다.

### 결과에 따른 다음 단계

| 로그 | 뜻 | 다음 |
|---|---|---|
| `about usb:hs audio ... INTERCEPT`가 없음 | audio가 이 도구보다 먼저 usb:hs에 연결했거나 다른 서비스를 씀 | 첫 줄의 부팅 후 시각과 hid 질문 여부를 확인. `config.ini`에 `services=usb:hs,usb:hs:a`, `mitm.lst`에 `usb:hs:a` 줄을 추가해 재시험 |
| audio가 Acquire만 하고 컨트롤 전송이 없음 | 디스크립터를 읽기 전에(인터페이스 정보만 보고) 포기 | 인터페이스 정보 중 무엇이 다른지 비교 → C에서 Query/Acquire 결과를 바꿈 |
| GET_DESCRIPTOR 뒤에 SET_INTERFACE/SET_CUR 없이 멈춤 | 디스크립터 해석 단계에서 거부 (H1~H7 중 하나) | 받은 디스크립터 바이트로 원인 확정 → C에서 디스크립터 재작성 |
| 어떤 컨트롤 요청이 실패(`res` 오류)하고 멈춤 | 장치가 그 요청을 거부(STALL) | C에서 그 요청을 성공으로 대답하거나 바꿈 |
| OpenUsbEp·BatchBufferAsync까지 진행 | 드라이버는 재생을 시작함 → 출력 장치 등록 단계 문제 | 전송 결과·보고를 확인 |
| `cannot read the client's memory` | 디버거 방식 읽기가 막힘 | 다른 기록은 그대로 유효. C의 디스크립터 재작성은 다른 방법이 필요 |
| 게임 소리가 끊기거나 audio 관련 오류 | 메모리 읽기(디버거 연결)가 audio를 방해 | `read_ctrl_data=0`으로 재시험 |

## 3단계 (v0.2.0): 디스크립터 고쳐 쓰기 — 접근 C

C-0 결과, 닌텐도 드라이버는 이어폰의 디스크립터를 읽고 재생 인터페이스를 잡은 뒤 **출력 장치로 등록하지 않고 멈췄다.** 실패한 요청은 없었다. 그래서 드라이버가 디스크립터를 해석하기 직전에 audio의 메모리에 있는 디스크립터를 고쳐 쓴다. 길이는 바꾸지 않는다.

| 규칙 (`rewrite=`) | 바꾸는 것 | 가설 |
|---|---|---|
| `terminal` (기본) | 재생 출력 단자 OT6 `0x0402`(Headset) → `0x0302`(Headphones), 마이크 입력 단자 IT1 `0x0402` → `0x0201`(Microphone) | H2 |
| `single_rate` | 재생 alt1의 샘플레이트 목록(48k, 96k) → 48k, 48k | H3 |
| `alt1_only` | 24·32비트 재생 모드(IF2 alt 2, 3)를 오디오가 아닌 것으로 표시 | 여분 포맷 |
| `mps48` | 재생 엔드포인트 최대 패킷 384 → 192(48 kHz 1 ms) | 패킷 크기 |
| `none` | 고치지 않음(C-0처럼 기록만) | |

audio가 EO-IC100(`04e8:a054/a059/a05b`)의 `GET_DESCRIPTOR(CONFIG)`를 받을 때만 고칩니다. 다른 USB 장치와 홈브루는 건드리지 않습니다. 이어폰 펌웨어도 바뀌지 않습니다(뽑으면 원래대로).

**시험 순서**
1. v0.2.0의 `exefs.nsp`와 `config.ini`를 SD에 넣습니다(eoaudio는 계속 끈 상태). `mitm.lst`는 그대로 둡니다.
2. 재부팅 → 게임 실행 → 이어폰을 귀에서 조금 떼고 꽂습니다. 10초 기다립니다.
3. 소리가 어디서 나는지 확인합니다. 콘솔 볼륨 버튼을 눌러 화면의 볼륨 아이콘이 헤드폰 모양으로 바뀌는지도 봅니다.
4. 소리가 이어폰으로 나오면 1분쯤 들어 보고(끊김, 속도, 좌우), 리모컨 볼륨도 눌러 봅니다. 홈 화면으로 나가서도 소리(메뉴 효과음)가 나는지 봅니다.
5. 이어폰을 뽑고 소리가 스피커로 돌아오는지 봅니다.
6. 로그 `switch/usbhs-logmitm/logs/`(이제 DBI MTP로 바로 복사됨)와 메모를 보내 주세요.

**로그에서 볼 줄**
- `rewrite: @087 output terminal 6: type 0x0402 -> 0x0302 (Headphones)`와 `written to audio's buffer: OK, read back the same` → 고쳐 쓰기가 됐습니다.
- 그 뒤에 `SetInterface: in [01]`, `CtrlXferAsync: OUT class ep UAC SET_CUR ... SAMPLING_FREQ`, `OpenUsbEp`, `BatchBufferAsync`가 나오면 드라이버가 재생을 시작한 것입니다.
- 여전히 SetInterface(alt 0) 뒤에 아무것도 없으면 H2가 아닙니다. 다음은 `rewrite=terminal,single_rate,alt1_only` 식으로 규칙을 하나씩 더해 시험합니다(재부팅 필요).

**문제가 생기면**: 소리가 이상하게(빠르게, 잡음) 나면 이어폰을 바로 뽑습니다. 콘솔이 오류를 내거나 소리가 아예 안 나면 `rewrite=none`으로 되돌리고 재부팅합니다.

## 설정 (`switch/usbhs-logmitm/config.ini`, 재부팅해야 적용)

| 키 | 기본값 | 뜻 |
|---|---|---|
| `enabled` | `1` | `0`이면 아무것도 가로채지 않음 |
| `services` | `usb:hs` | 가로챌 서비스(`usb:hs`, `usb:hs:a`). 부팅 가로채기를 하려면 `mitm.lst`에도 같은 이름을 한 줄씩 적어야 함 |
| `programs` | `audio` (1단계 검증 때 `audio,homebrew`) | 가로챌 프로그램: `audio`, `hid`, `homebrew`(hbmenu로 띄운 NRO), `all`, 16자리 ID |
| `read_ctrl_data` | `0` (진단 때 `1`) | 컨트롤 전송 데이터(디스크립터 응답, SET_CUR 값) 기록 |
| `sync` | `0` (진단 때 `1`) | 줄마다 SD에 기록한 뒤 요청을 넘김(느리지만 재부팅에도 남음) |
| `hot_detail` | `40` | isoc·프레임 번호 요청을 세션마다 자세히 남길 개수 |
| `buf_dump` | `64` | 해석하지 못한 IPC 버퍼를 16진수로 남길 최대 바이트 |
| `log_keep` | `20` | 남겨 둘 로그 파일 수 |
| `rewrite` | `terminal` | 접근 C 규칙(위 3단계 표). `none`이면 기록만 |
| `remote` | `0` | 이어폰 리모컨 볼륨(실험). hid가 받은 리모컨 리포트를 읽어 볼륨을 바꾼다. `programs`에 `hid`가 있어야 하고, hid는 부팅 때 연결하므로 `mitm.lst`도 필요 |

`programs=all`이나 `hid`는 권하지 않습니다. hid는 컨트롤러 입력이 이 도구를 거치게 됩니다.

## 평소 사용

- 동작이 확인된 뒤의 기본 `config.ini`는 `read_ctrl_data=0`, `sync=0`이다. 디스크립터 고쳐 쓰기(`rewrite=terminal`)는 그대로 하고, 볼륨을 바꿀 때마다 audio에 붙어 데이터를 읽거나 줄마다 SD에 즉시 쓰는 진단 동작만 끈다. 로그 파일은 계속 남는다(연결/요청 순서, 5초마다 전송 개수).
- Ultrahand는 필요 없다. 부팅할 때 자동으로 시작하고, 설정은 `config.ini`를 고친 뒤 재부팅하면 적용된다. 실행 중에 끄면 sm fatal이 날 수 있어서 Ultrahand/ovl-sysmodules 목록에 일부러 넣지 않았다.

**hid 가로채기에 대해:** `programs=audio,hid`면 스위치 입력 모듈(hid)의 USB 통신도 이 sysmodule을 거쳐 간다. 이어폰 리모컨 외의 장치는 그대로 전달한다. USB 컨트롤러가 이상하면 `programs=audio`로 되돌리고 재부팅한다(리모컨 볼륨만 꺼짐).

## 끄기 / 제거

- **부팅 가로채기만 끄기**: `atmosphere/contents/420000000E01C0C0/mitm.lst`를 지우고 재부팅(1단계 상태로 돌아감).
- **완전히 끄기**: `atmosphere/contents/420000000E01C0C0/flags/boot2.flag`를 지우고 재부팅.
- **제거**: `atmosphere/contents/420000000E01C0C0` 폴더와 `switch/usbhs-logmitm` 폴더를 지우고 재부팅.
- 부팅이 멈추면: 전원 12초 → Hekate UMS로 `atmosphere/contents/420000000E01C0C0` 삭제(위 "복구 방법").
- 실행 중에 끄지 마세요(ovl-sysmodules 등). 위 주의 참고.

## 안전장치와 알려진 위험

- 가로채기 대상은 audio와 홈브루뿐입니다. hid·bsdsockets·eoaudio 등은 sm에 "가로채지 않음"으로 대답해 원래 서비스로 바로 갑니다.
- 설치 직후 `ClearFutureMitm`을 보냅니다(eoaudio v0.4.3에서 확인된 필수 단계). 설치에 실패해도 보내서, `mitm.lst` 선언 때문에 클라이언트가 영원히 기다리지 않게 합니다.
- sm의 질문은 전용 스레드가, 설치는 메인 스레드가 합니다(교착 방지). sm과는 전용 `sm:` 세션(RegisterClient)으로 이야기합니다.
- 크래시 때는 예외 처리기가 MITM을 sm에서 먼저 내리고 `logs/<실행>_crash.txt`를 남깁니다. 그래도 이미 가로챈 audio의 세션은 함께 끊기므로 audio가 오류를 낼 수 있습니다(재부팅으로 복구).
- `mitm.lst`가 있는 상태에서 이 도구가 시작되지 못하면 hid·audio가 기다린 채로 부팅이 멈춥니다. 위 복구 방법으로 폴더를 지우면 됩니다.
- `read_ctrl_data=1`은 컨트롤 전송마다 audio 프로세스에 디버거로 잠깐 붙었다 떨어집니다(dmnt 치트와 같은 방식). 장치를 꽂을 때 수십 번 정도입니다.

## 빌드

WSL devkitPro(`/opt/devkitpro`, libnx 4.12):

```bash
make -C switch/usbhs-logmitm            # usbhslog.nsp + sd/ 배치
make -C switch/usbhs-logmitm/tests check # 호스트 테스트 (setup packet 해석, 설정 파서, 실기기 디스크립터로 재작성 규칙)
```

소스: `source/mitm.c`(sm·전달·기록·고쳐 쓰기), `source/rewrite.c`(디스크립터 규칙), `source/decode.c`(명령 이름, setup packet·디스크립터 해석), `source/config.c`, `source/log.c`(eoaudio 로거 변형: 로그 파일이 열리기 전 줄도 보관), `source/crash.c`, `source/main.c`.
