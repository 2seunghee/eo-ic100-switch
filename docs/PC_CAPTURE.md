# (선택) PC에서 초기화 시퀀스 캡처하기

목적: **정상 동작하는 호스트(Windows)** 가 EO-IC100을 어떻게 초기화하는지를 기준 자료로 남깁니다.
나중에 C-0 로깅 MITM(docs/PLAN.md §5)으로 얻을 스위치 쪽 요청과 나란히 비교하는 데 씁니다.
디스크립터 자체는 `pc/dump_usb_descriptors.ps1`이 이미 얻어 주므로, 이 단계는 우선순위가 낮습니다.

## 준비
- [Wireshark](https://www.wireshark.org/) 설치 시 **USBPcap** 구성 요소를 함께 선택하고 재부팅합니다.
- 소리 설정 → 이어폰 → 속성 → 고급 → 기본 형식을 **16비트, 48000 Hz**로 맞춥니다(스위치와 같은 조건).

## 캡처
1. Wireshark에서 `USBPcap1`, `USBPcap2`… 중 이어폰이 연결될 루트 허브를 고릅니다. 모르면 하나씩 시험합니다.
2. 캡처를 시작한 **뒤에** 이어폰을 꽂습니다. 열거 과정 전체가 기록됩니다.
3. 소리 설정의 "테스트" 버튼으로 몇 초 재생하고, 볼륨을 한 번 바꾸고, 리모컨 버튼을 한 번 누릅니다.
4. 캡처를 멈추고 `eo-ic100_windows.pcapng`로 저장합니다.

## 볼 곳
| 확인할 것 | 표시 필터(예) | 기대값 |
|---|---|---|
| 장치 주소 찾기 | `usb.idVendor == 0x04e8` | 응답 패킷의 장치 주소를 확인한 뒤 `usb.device_address == N`으로 좁힌다 |
| 컨트롤 전송만 | `usb.transfer_type == 0x02` | |
| SET_INTERFACE | `usb.setup.bRequest == 11` | 재생 시작 시 IF2 alt1, 정지 시 alt0 |
| 샘플레이트 설정 | `usb.setup.bRequest == 1` 중 bmRequestType 0x22 | wIndex 0x0003(EP), 데이터 `80 bb 00`(48000) |
| 볼륨/뮤트 | bmRequestType 0x21 / 0xa1 | wIndex 0x0200(FU id2, IF0) |
| isoc 데이터 | `usb.transfer_type == 0x00` | 1 ms마다 192바이트 OUT |
| 리모컨 | `usb.transfer_type == 0x01` | EP 0x84 IN 3바이트 리포트 |

> 필드 이름은 Wireshark 버전에 따라 조금씩 다릅니다. 패킷 상세 창에서 해당 필드를 우클릭한 뒤 "Apply as Filter"로 정확한 이름을 확인하세요.

회신할 것: `.pcapng` 파일. 또는 위 표의 요청들을 시간 순으로 적은 메모.
