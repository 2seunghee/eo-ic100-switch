# Samsung EO-IC100 on Nintendo Switch

탈옥(Atmosphère) 닌텐도 스위치가 삼성 정품 USB-C 이어폰 **EO-IC100**(UAC1 디지털 이어폰)을 USB 오디오 출력으로 쓰게 만든 프로젝트입니다.

## 바로 쓰기

**[Releases](../../releases)** 에서 `eo-ic100-usbaudio-v*.zip`을 받아 SD 카드 루트에 풀고 재부팅하면 됩니다. 설치·제거·복구 방법은 zip 안의 `README.txt`([switch/usbhs-logmitm/dist/README.txt](switch/usbhs-logmitm/dist/README.txt))에 있습니다.

- 이어폰을 꽂으면 스위치가 "USB 스피커"로 인식하고, 게임·홈 화면·홈브루 소리가 모두 이어폰으로 나옵니다. 콘솔 볼륨도 그대로 동작합니다.
- 확인된 환경: HOS 22.1.0 + Atmosphère, EO-IC100 `04e8:a05b`. `a054`/`a059` 변형, 리모컨 버튼, 마이크, 슬립 후 복귀, TV 모드는 아직 확인하지 않았습니다.

## 원리

스위치의 audio sysmodule은 이 이어폰의 디스크립터를 읽고 재생 인터페이스까지 잡지만, 출력 장치로 등록하지 않았습니다. 원인은 출력·입력 단자 종류가 `0x0402`(Headset, 입출력 겸용)로 적혀 있는 것이었습니다(가설 H2).

[switch/usbhs-logmitm](switch/usbhs-logmitm/README.md)은 Atmosphère MITM으로 audio와 `usb:hs` 사이에 끼어듭니다. audio가 받은 설정 디스크립터의 단자 종류를 `0x0302`(Headphones)와 `0x0201`(Microphone)로 고쳐 씁니다(3바이트, 디버거 방식 메모리 쓰기). 나머지 요청은 그대로 전달합니다. 그 뒤로는 닌텐도 드라이버가 볼륨 설정, 48 kHz 설정, isoc 스트리밍을 직접 합니다. 조사 과정과 실기기 로그 해석은 **[docs/PLAN.md](docs/PLAN.md)** §0에 있습니다.

## 진행 기록

- **접근 A** (eoprobe, NRO): 홈브루에서 isoc 전송으로 이어폰에 직접 소리를 냈다. isoc 규약은 `BatchBufferAsync(unk1 = 0, unk2 = 절대 시작 프레임)`.
- **접근 B** (eoaudio, sysmodule): 게임 최종 믹스를 녹음해 이어폰으로 보냈다. 홈 화면과 audout 소리는 녹음되지 않아 한계가 있었다. 지금은 쓰지 않는다.
- **접근 C-0 → C** (usbhs-logmitm): 드라이버가 멈추는 지점을 기록하고, 디스크립터를 고쳐 써서 해결했다.
- **FM 라디오** ([switch/eofm](switch/eofm/README.md)): 이어폰의 내장 FM 칩을 벤더 요청으로 조작하는 한글 앱. 이어폰 전용이며 채널 찾기, 즐겨찾기가 실기기에서 동작한다. 본체 스피커로 듣기는 오디오 모듈을 중단시켜 뺐다.
- **리모컨 볼륨** (usbhs-logmitm 0.3.1, 확인 대기): 드라이버가 hid에게서 리모컨을 숨기고 직접 읽어 볼륨을 바꾼다.

## 구성

```
pc/
  dump_usb_descriptors.ps1   Step 1: Windows에서 이어폰 상태 확인 + 디스크립터 덤프(드라이버 교체 없음)
  uacdesc.py                 디스크립터 분석기: show / check(H1~H7) / diff / json / usbhs
  tests/                     Python 단위 테스트
switch/eoprobe/              Step 2·3: 스위치 NRO (libnx)
  source/                    probe(usb:hs 스캔), isoc(접근 A 스트리밍), uac, hidmon, sysinfo, usbdesc(파서)
  tests/                     usbdesc.c 호스트 단위 테스트
switch/eoaudio/              접근 B: 게임 소리 → 이어폰 sysmodule (libnx, ExeFS .nsp)
  source/                    recorder(audrec), isoc(연속 전송), pcm(지터 버퍼·드리프트 보정), device(장치 확보), policy(스피커 끄기·물러나기), remote, speaker
  tests/                     pcm·config·policy 호스트 단위 테스트
switch/usbhs-logmitm/        접근 C: usb:hs MITM sysmodule (기록 + 디스크립터 고쳐 쓰기). 배포 zip의 내용물
  dist/README.txt            배포 zip에 들어가는 사용자 설명서
switch/eofm/                 EO-IC100 내장 FM 라디오 앱(NRO, 한글 화면, 이어폰 전용). attic/: 시도했다 뺀 스피커 출력
fixtures/                    공개 lsusb 덤프에서 복원한 EO-IC100 디스크립터
docs/                        PLAN.md, PC_CAPTURE.md
dumps/                       실기기 디스크립터 덤프와 usbhs-logmitm 로그(C-0/C 근거)
```

## 진단 도구 사용법 (개발용, 접근 A 때 쓰던 순서)

1. **PC 확인**: 이어폰을 PC에 꽂고 실행합니다.
   ```powershell
   powershell -ExecutionPolicy Bypass -File pc\dump_usb_descriptors.ps1
   ```
   결과는 `dumps\`에 저장됩니다. 플러그 유격이 의심되면 `-Watch 1`을 붙입니다.
2. **스위치 진단**: `switch/eoprobe`를 빌드한 `eoprobe.nro`를 SD의 `/switch/eoprobe/`에 넣고, 이어폰을 꽂은 상태로 hbmenu에서 실행합니다. 실행하면 자동으로 스캔합니다.
3. **소리 시험**(접근 A): IF2가 `held by another process`이면 먼저 **R**(race)을 누르고 이어폰을 다시 꽂습니다. 이어폰을 귀에서 조금 뗀 상태로 **X**(연속 톤, B로 정지)를 누릅니다. **ZL**은 좌우 채널 테스트입니다.
4. SD의 `/switch/eoprobe/` 폴더와 PC의 `dumps\`를, 들은 결과 메모와 함께 보내 주세요.

| 버튼 | 동작 |
|---|---|
| A | 재스캔 + 디스크립터 덤프 |
| X / Y | 톤 재생(B로 정지, 최대 60초) / BatchBufferAsync 전략 스윕 |
| R | race-acquire(재연결되는 순간 인터페이스 선점) |
| L | HID 리모컨 버튼 모니터 |
| ZR | 장치 리셋 (usbHsIfResetDevice) |
| ↑↓ / ←→ / ZL | 레벨(dBFS) / 전략 / 좌우 채널 테스트 |
| − / + | 다음 장치 / 종료 |

> ⚠️ 스윕은 검증되지 않은 인자로 usb sysmodule을 호출하므로 시스템 크래시(재부팅)가 날 수 있습니다. 게임을 종료하고 실행하세요. 로그는 매 전략 직전에 SD에 fsync됩니다.

## 분석기

```bash
python pc/uacdesc.py fixtures/eo-ic100_04e8_a054_config.hex      # show + check
python pc/uacdesc.py diff dumps/<working>_config.bin dumps/04e8_a054_config.bin
python pc/uacdesc.py usbhs <SD>/switch/eoprobe/usbhs_query_all.bin
python pc/uacdesc.py hid fixtures/eo-ic100_04e8_a05b_hid_if4.hex          # HID 리포트 디스크립터 해석 + 규격 검사
```

PC 덤퍼는 HID 리포트 디스크립터(`*_hid_if<N>.bin`)도 함께 저장합니다.

## 빌드와 테스트

- NRO: [devkitPro](https://devkitpro.org/wiki/Getting_Started) switch-dev(libnx 4.12, devkitA64 GCC 15.2로 확인)
  ```bash
  cd switch/eoprobe && make
  ```
- sysmodule(eoaudio): `eoaudio.nsp`와 SD 배치(`sd/atmosphere/contents/420000000E01C100/`)가 만들어집니다.
  ```bash
  make -C switch/eoaudio
  ```
- 테스트
  ```bash
  python -m unittest discover -s pc/tests
  ```
  ```bash
  make -C switch/eoprobe/tests check
  ```
  ```bash
  make -C switch/eoaudio/tests check
  ```
- 배포 sysmodule(usbhs-logmitm): `usbhslog.nsp`와 SD 배치(`sd/atmosphere/contents/420000000E01C0C0/`)가 만들어집니다.
  ```bash
  make -C switch/usbhs-logmitm
  ```
  ```bash
  make -C switch/usbhs-logmitm/tests check
  ```

## 참고
- [switchbrew: USB services](https://switchbrew.org/wiki/USB_services): usb:hs 명령, 필터 규칙, audio sysmodule의 usb:hs 사용
- [libnx usbhs.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/usbhs.h)와 공식 예제 `examples/switch/usb/usbhs`
- [kjy00302/eo-ic100_radio](https://github.com/kjy00302/eo-ic100_radio): EO-IC100 FM 라디오 프로토콜, PID 변형
- [linux-hardware.org probe 7bc1964f22](https://linux-hardware.org/?probe=7bc1964f22&log=lsusb): fixture의 출처
