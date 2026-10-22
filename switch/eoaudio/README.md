# eoaudio: 스위치 소리를 EO-IC100으로 (접근 B sysmodule)

탈옥(Atmosphère) 스위치에서 백그라운드로 도는 sysmodule이다. 시스템의 최종 오디오 믹스(게임, HOME 메뉴, 모든 소리)를 libnx `audrec` FinalOutputRecorder로 받아서, eoprobe에서 확인한 isoc 전송으로 Samsung EO-IC100 USB-C 이어폰에 직접 보낸다. 계획과 실기기 결과는 [docs/PLAN.md](../../docs/PLAN.md) §0, §5 B에 있다.

> **현재 상태 (v0.2.0, 2026-10-20):** devkitA64로 경고 없이 빌드되고 호스트 단위 테스트는 모두 통과한다. **실기기에서는 아직 한 번도 실행되지 않았다.** 아래 기능은 모두 "잘 동작한다는 전제" 위에 만들었고, 문제가 생기면 로그로 원인을 찾을 수 있게 해 두었다.

> **충돌 주의 (2026-10-22):** 접근 C-0 도구 [switch/usbhs-logmitm](../usbhs-logmitm/README.md)으로 시험하는 동안에는 eoaudio를 꺼 둡니다(`atmosphere/contents/420000000E01C100/flags/boot2.flag` 삭제 후 재부팅). eoaudio가 꽂는 순간 인터페이스를 모두 선점하면 닌텐도 드라이버의 동작을 기록할 수 없습니다.

## v0.5.0 (2026-10-22): audout 가로채기 제거

- v0.4.7 실기기 결과: 앨범으로 띄운 YTMusicNX의 audout 세션을 가로채는 데까지는 성공했다. 하지만 소리 버퍼를 읽는 `svcMapProcessMemory`가 `0xD401`로 실패했다. 버퍼는 일반 힙(type 0x5)이고, 커널이 다른 프로세스의 매핑을 허용하지 않는다. 그래서 audout 소리는 이 방식으로 이어폰에 보낼 수 없다고 결론 내리고 MITM 코드(`mitm.c`, Ultrahand 항목)를 뺐다.
- 예전 `config.ini`에 남은 `audout_mitm` 줄은 경고 없이 무시한다.
- 홈 화면 소리와 audout 홈브루 소리까지 이어폰으로 보내려면, 스위치 자체의 USB 오디오 출력이 이어폰을 받아들이게 해야 한다(docs/PLAN.md의 접근 C).

## v0.4.7 (2026-10-22)

- v0.4.6 실기기 결과: 이어폰을 꽂자 MITM이 설치됐고, 빼자 해제됐다. sm fatal은 없었다. 하지만 섞인 소리는 없었다.
  - 이미 재생 중이던 플레이어는 설치 전에 audout을 열어 두었기 때문에 가로챌 수 없었다.
  - 그사이 audout을 연 프로그램은 앨범 애플릿(`010000000000100d`) 하나뿐이었다. hbmenu가 홈브루를 여기서 실행하므로, 이것은 YTMusicNX 창이었다. 이 애플릿은 "시스템 프로그램"이라서 건드리지 않았다.
- Atmosphère override 플래그의 bit 0(hbl)이 켜진 프로세스는 시스템 프로그램 ID라도 가로챈다. 앨범으로 띄운 홈브루가 여기에 해당한다. 추적 줄에 플래그도 남긴다.
- 사용법: 이어폰을 꽂은 **뒤에** 재생을 시작하거나, 일시정지했다가 다시 재생한다. 플레이어가 audout을 새로 열어야 섞인다.

## v0.4.6 (2026-10-22): sm fatal 방지

- 실기기 결과: MITM이 설치된 채로 Ultrahand → Sysmodules에서 eoaudio를 껐다. 그다음 audout:u 클라이언트가 생기자 sm(`0100000000000004`)이 `std::abort()`로 fatal을 냈다. sm은 MITM 프로세스에 ShouldMitm을 묻고 실패하면 abort한다(`R_ABORT_UNLESS`). 앞선 "재부팅"도 같은 원인이었을 가능성이 크다.
- 이제 MITM은 **audout 섞기가 켜져 있고 이어폰이 재생 중일 때만** sm에 설치된다. 이어폰을 빼거나 설정을 끄면 바로 `AtmosphereUninstallMitm`(65001)으로 내린다. 이미 열린 세션은 앱이 닫을 때까지 계속 중계한다. 예외 처리기도 종료 전에 MITM을 내린다.
- 설치와 해제는 메인 스레드에서 sm을 호출한다. MITM 스레드는 sm의 질문에 답하는 일만 한다. 그래서 sm이 질문을 보낸 채 우리를 기다리는 동안 우리도 sm을 기다리는 교착이 생기지 않는다.
- **주의:** 이어폰을 꽂고 섞기를 켠 상태에서 eoaudio 프로세스를 끄면 여전히 sm fatal이 날 수 있다. eoaudio를 끄기 전에 이어폰을 빼거나 섞기를 끈다.

## v0.4.5 (2026-10-22): 재부팅 진단

- v0.4.4 실기기 결과: YTMusicNX를 닫은 뒤 오버레이 로더(`420000000007e51a`)가 audout을 멈추고 다시 열었다. 그 직후 콘솔이 재부팅됐다. 크래시 리포트와 fatal 리포트는 없었다. 로그는 마지막 요청 직후에서 끊겼다.
- MITM의 추적 줄(최대 400개)은 이제 줄마다 SD에 기록한 뒤에 요청을 넘긴다(`log_sync`). 전달하기 전("forwarding")과 결과("->")를 따로 남기고, 세션이 닫힐 때도 남긴다. 재부팅되면 마지막 줄이 재부팅 직전의 단계다.

## v0.4.4 (2026-10-21)

- v0.4.3 실기기 결과: 콘솔이 멈추지 않았다.
  - sm의 질문, AcceptSession, AcknowledgeMitmSession, OpenAudioOut 전달, 세션 교체가 모두 OK였다.
  - YTMusicNX는 Tesla 오버레이 로더(`420000000007e51a`, nx-ovlloader) 프로세스 안에서 audout으로 재생한다.
  - 시스템 프로그램(`010000000000100d`)은 의도대로 건드리지 않았다.
- 남은 문제: 소리 버퍼를 읽는 `svcMapProcessMemory`가 `0xD401`(InvalidCurrentMemory)로 실패한다.
- 이번 버전의 변경:
  - 요청 중에 실패하면 응답을 보낸 뒤 한 번 더 시도한다. 요청 중에는 AudioOutBuffer 구조체와 같은 페이지가 IPC로 잠겨 있을 수 있다.
  - 실패하면 그 메모리의 type, attr, perm(`svcQueryProcessMemory`)을 로그에 남겨 다음 방법을 정한다.

## v0.4.3 (2026-10-21)

- **v0.4.0~0.4.2가 콘솔을 멈추게 한 원인을 고쳤다.** Atmosphère sm은 `InstallMitm`을 받으면 "곧 MITM이 생긴다"는 future 선언을 남긴다. 그리고 `ClearFutureMitm`(65007)이 올 때까지 그 서비스의 `GetServiceHandle`을 모두 미룬다. eoaudio는 이걸 지우지 않아서, audout:u를 쓰려는 모든 프로그램(YTMusicNX, YouTube, 홈 메뉴)이 끝없이 기다렸다. 실기기 로그에서 sm의 질문이 한 번도 오지 않았고, YTMusicNX `app.log`는 `audout deferred` 뒤로 멈춰 있었다. 이제 스레드를 띄운 뒤 `ClearFutureMitm`을 보낸다(libstratosphere와 같다).

## v0.4.2 (2026-10-21)

- 시작할 때 `config.ini`의 `audout_mitm=1`을 0으로 되돌려 쓴다. v0.4.1에서는 파일에 이미 1이 있으면 Ultrahand에서 켜도 파일이 바뀌지 않아 설치되지 않았다(실기기 로그 17:18, 17:21). 이제는 Ultrahand 토글이 항상 꺼짐에서 시작하고, 켜면 설치된다.

## v0.4.1 (2026-10-21): audout 섞기는 직접 켤 때만

- v0.4.0의 실기기 결과: MITM은 설치됐다. 하지만 audout:u에 연결된 프로그램이 하나도 없었고, 그 뒤 콘솔이 멈춰서 부팅도 되지 않았다. 기존 `config.ini`에 `audout_mitm` 줄이 없어서 코드 기본값(1)이 쓰였기 때문이다.
- `audout_mitm`의 기본값을 0으로 바꿨다. **부팅할 때는 1이어도 설치하지 않는다.** eoaudio가 도는 중에 0에서 1로 바꿀 때만 설치한다(Ultrahand → eoaudio → "audout 소리 섞기 (실험)"). 설치가 잘못돼도 재부팅하면 정상으로 돌아온다.
- 시스템 프로그램(`0100000000000000`~`0100000000001FFF`: 홈 메뉴, 애플릿, sysmodule)은 가로채지 않는다.
- sm과는 libstratosphere처럼 MITM 전용 세션으로 통신한다(`sm:` 연결 + RegisterClient).
- 처음 60단계를 info 수준으로 남긴다: sm의 질문과 답, AcceptSession, AcknowledgeMitmSession, 전달한 명령.

## v0.4.0 (2026-10-21): audout 소리 섞기 (실험)

- 녹음(FinalOutputRecorder)에는 audren으로 나오는 게임 소리만 들어온다. audout으로 나오는 소리는 들어오지 않는다. YTMusicNX 같은 홈브루 음악 플레이어는 audout을 쓰고, 백그라운드 프로세스에서 재생하는 경우도 있다.
- eoaudio가 Atmosphère MITM으로 `audout:u` 앞에 선다(`source/mitm.c`). 모든 요청은 실제 서비스에 그대로 넘긴다. `AppendAudioOutBuffer`가 올 때마다 그 버퍼의 PCM을 클라이언트 메모리에서 읽어(`svcMapProcessMemory`) 보조 링에 넣는다. 스트림은 이 링을 게임 소리와 섞어 이어폰으로 보낸다. 이어폰 볼륨은 둘 다에 적용된다.
- audout 세션은 2개까지 받는다(각 683 ms 링). 500 ms 넘게 쌓이면 앞부분을 버려 지연을 줄인다. CMIF 도메인으로 바뀐 세션은 넘기기만 하고 섞지 않는다.
- 로그 줄:
  - `audout mitm: installed`
  - `... connected to audout:u`: audout을 연 프로그램. 홈 화면(`0100000000001000`)이 여기에 나오면 홈 화면 소리도 섞인다.
  - `... opened an audio out ... -> mixed into the earphone`
  - `... plays: first buffer`
  - health 줄 끝의 `audout mitm: ... ms copied, ... ms queued`
- `audout_mitm=0`이면 끈다. 다시 시작해야 적용된다. eoaudio가 비정상 종료하면 audout을 쓰는 앱의 소리가 끊길 수 있다. 그때는 `audout_mitm=0`으로 두고 다시 켠다.

## v0.3.1 (2026-10-21)

- v0.3.0에서 볼륨 표시창이 보이지 않던 것을 고쳤다. 프레임버퍼를 직접 만들면서 libnx `framebufferCreate`가 하던 `nwindowSetDimensions`를 빠뜨렸다. 그래서 `framebufferEnd`가 0줄을 복사해 투명한 화면만 나갔다.
- v0.3.0 실기기 로그 기준으로, 평소 eoaudio 메모리가 6.4 MB에서 1.7 MB(힙 사용 약 200 KB)로 줄었다. system pool 여유는 5.3 MB에서 7.9 MB로 늘었다.

## v0.3.0 (2026-10-21): 메모리

- v0.2.9 로그에서 system pool의 여유가 5.3 MB(전체 232 MB)뿐이었고, 그중 eoaudio가 6.4 MB를 쓰고 있었다. system pool은 모든 sysmodule과 Tesla/Ultrahand 오버레이가 함께 쓴다. Ultrahand 메뉴가 빈 화면으로 뜬 것은 이 메모리 부족 때문이었을 가능성이 크다.
- 고정 힙을 3.5 MB에서 1 MB로 줄였다. 볼륨 표시창의 프레임버퍼 2 MB는 표시창이 떠 있는 동안에만 system pool에서 빌리고(`svcSetHeapSize`), 사라지면 돌려준다. 평소에는 약 4 MB를 덜 쓴다. 표시창을 띄울 메모리가 그 순간 없으면 `osd: no memory for the volume bar now` 줄을 남기고 그 표시만 건너뛴다.
- `memory (...)` 줄에 eoaudio 힙의 사용량도 남긴다.
- 홈 화면에서는 이어폰에 소리가 나지 않는다. v0.2.9 로그에서 게임이 없는 동안 녹음은 마스터 볼륨 0 방식이든 음소거 방식이든 처음부터 끝까지 디지털 무음이었다. FinalOutputRecorder에는 게임(애플리케이션)의 소리만 들어오고, 홈 메뉴 같은 시스템 소리는 들어오지 않는다. 스위치의 동영상 캡처에 게임 소리만 녹음되는 것과 같다.

## v0.2.9 (2026-10-21)

- 시작할 때와 1분마다 `memory (...): system pool N KiB free of M, applet pool ...; eoaudio uses K KiB` 줄을 남긴다. Tesla/Ultrahand 오버레이와 sysmodule은 system pool을 함께 쓴다. 메모리가 모자라 보일 때 eoaudio가 얼마를 쓰는지 이 줄로 비교한다.

## v0.2.8 (2026-10-21)

- 콘솔 볼륨에 묶기(`volume_follow=1`)에서 콘솔 볼륨 0칸이면 이어폰도 무음이다. 전에는 −42 dB로 아주 작게 소리가 났다.
- 같은 모드에서 리모컨 Vol+/Vol−를 누르면 이어폰 볼륨 표시창이 뜬다. eoaudio가 콘솔 볼륨을 바꿔도 시스템 볼륨 표시는 나오지 않기 때문이다. 본체 버튼을 누르면 지금처럼 시스템 표시가 뜬다.

## v0.2.7 (2026-10-21)

- 볼륨 표시창은 보일 때만 레이어를 만든다. 1.5초 뒤 사라지면 레이어와 vi:m, nvdrv:s 세션을 모두 닫는다. v0.2.6은 레이어를 계속 열어 두었고, 그 상태에서 Ultrahand(Tesla) 메뉴가 빈 화면으로 떠서 재부팅해야 했던 일이 있었다.
- 꽂았을 때 스피커가 더 빨리 꺼진다. 스트림이 약 50 ms(전에는 200 ms) 정상으로 돌면 바로 끄고, 이어폰이 꽂혀 있는 동안에는 상태를 0.05초(전에는 0.25초)마다 본다. 꽂은 뒤 기다리는 `settle_ms`(기본 200)도 줄이면 더 빨라지지만, 그동안 이어폰이 다시 연결되는 일이 생길 수 있다.
- 뺐을 때 스피커가 더 빨리 돌아온다. 전송이 0.1초 동안 계속 실패하면 바로 스피커를 되돌린다(전에는 전송을 포기하는 1초 뒤에 되돌렸다).
- 본체 볼륨 버튼을 누르면 스위치 자체의 볼륨 표시가 계속 움직인다. 시스템이 그리는 표시라 eoaudio가 막을 수 없다. 실제로는 마스터 볼륨이 0이라 스피커 소리는 나지 않는다. 본체 볼륨 버튼으로 이어폰 볼륨을 조절하고 싶으면 `volume_follow=1`을 켠다.

## v0.2.4 ~ v0.2.6 (2026-10-21, 실기기 로그 기반)

- 스피커 끄기 `auto`의 첫 방법이 **마스터 볼륨 0**이다(`speaker_mute=master0`도 가능). 본체 볼륨 버튼은 스피커 볼륨을 바꾸면서 음소거를 풀지만 마스터 볼륨은 건드리지 않아, 소리가 새지 않는다(v0.2.5 실기기 확인, `verdict.speaker.master0=kept`). 음소거 방식으로 돌아간 경우에도 볼륨 버튼으로 풀린 음소거는 횟수 제한 없이 0.05초 간격으로 다시 건다.
- 다시 꽂았을 때 스피커가 꺼지기까지의 시간을 약 1.8초에서 0.2~0.3초로 줄였다. 녹음기가 살아 있는지를 1초 통계 대신 프레임 카운터로 바로 보고, 스트림 0.2초 뒤에 기억된 방법을 적용한다. 녹음이 5초(전에는 3초) 동안 끊길 때만 스피커를 되돌린다(앱을 닫을 때 잠깐 스피커가 켜지던 것).
- 볼륨 표시창 레이어를 Tesla와 똑같이 만든다: 448×720 프레임버퍼, 화면 왼쪽 띠(672×1080) 레이어, Tesla가 쓰는 레이어 스택 8개, vsync를 기다린 뒤 표시. 그래서 표시창이 스크린샷과 녹화에도 찍힌다. 메모리는 약 4 MB를 쓴다.
- 본체 볼륨에 이어폰 볼륨을 묶고 싶으면 Ultrahand의 "콘솔 볼륨에 묶기"(`volume_follow=1`)를 켠다. 마스터 볼륨 0 방식에서도 동작하고, 녹음은 스피커 볼륨의 영향을 받지 않으므로 두 번 적용되지 않는다.

## v0.2.3 (2026-10-21): 실시간 설정, Ultrahand

- eoaudio가 `config.ini`를 2초마다 다시 읽고 바뀐 값을 바로 적용한다. 다시 시작해야 하는 키는 `mode`, `target_ms`, `rec_*`, `pids`, `iso_*`, `start_delay_s`, `log_keep`, `mute_test`, `acquire`뿐이다. 나머지(스피커 끄기, 볼륨 범위/한 칸, 볼륨 표시창, 가운데 버튼, 미디어 앱, 로그 수준 등)는 즉시 적용된다.
- `enabled=0`이면 eoaudio가 바로 꺼진다(이어폰 재생을 멈추고 스피커를 되돌린다). `enabled=1`이면 다시 켜진다. 프로세스는 계속 돌기 때문에 재부팅이 필요 없다.
- **Ultrahand 패키지**(`/switch/.packages/eoaudio/package.ini`, zip에 들어 있음): Ultrahand → Packages → eoaudio에서 켜기/끄기, 스피커 끄기 방식, 가운데 버튼, 누를 버튼, 볼륨 표시창, 볼륨 한 칸, 최대 볼륨, 콘솔 볼륨에 묶기, 로그 수준을 바꾼다. "현재 앱을 미디어 앱으로"는 지금 실행 중인 앱을 `media_apps`로 지정한다(목록을 그 앱 하나로 바꾼다). Ultrahand에는 sysmodule 프로세스 자체를 켜고 끄는 명령이 없어서, 켜기/끄기는 `enabled`로 한다. 프로세스까지 내리려면 예전처럼 ovl-sysmodules를 쓴다.
- Ultrahand의 `set-ini-val`은 섹션이 필요해서 `config.ini`에 `[eoaudio]` 줄을 넣었다. 예전 파일에 없어도 Ultrahand가 섹션을 덧붙이고, eoaudio는 섹션 줄을 무시하며 같은 키가 두 번 있으면 뒤의 값을 쓴다.

## v0.2.2 (2026-10-21)

- **이어폰 볼륨은 콘솔 볼륨과 따로 움직인다**(`volume_follow=0`이 기본). 콘솔 볼륨처럼 15칸(0 ~ −42 dB, 한 칸 3 dB)이고, 맨 아래에서 한 번 더 내리면 0칸(무음)이다.
- **이어폰 볼륨 표시창**(`volume_osd=1`): 리모컨으로 볼륨을 바꾸면 화면 왼쪽 위에 콘솔 볼륨 표시와 같은 모양의 막대가 1.5초 뜬다. 스피커 대신 이어폰 아이콘이 그려지고, 일시정지나 0칸이면 아이콘에 빨간 줄이 그어진다. Tesla 오버레이처럼 vi 관리 레이어에 그리고, 숨길 때 레이어를 없앤다. 표시창을 만들지 못하면 로그에 `osd:` 줄이 남고, 3번 실패하면 그 실행에서는 더 시도하지 않는다.
- **가운데 버튼**(`play_button=media`가 기본): `media_apps`에 있는 앱이 실행 중일 때만 `media_button`(기본 A)을 한 번 누른다(재생/정지). 다른 앱이나 게임에서는 아무것도 하지 않는다. 버튼은 hid:dbg 가상 Joy-Con을 레일의 Joy-Con에 합쳐서(추가 컨트롤러 없이) 누르고 바로 떼어 낸다. 기본 목록은 YouTube(`01003A400C3DA000`)다. 다른 앱은 로그의 `app: application ... running` 줄에서 ID를 보고 `media_apps=ID1,ID2`처럼 더한다. 스위치에는 시스템 전체에 보내는 재생/정지 신호가 없어서, 앱마다 그 앱이 재생/정지로 쓰는 버튼을 누르는 방식이다.
- 설정 형식이 3으로 바뀌었다. 예전 `config.ini`는 `config.ini.v2.bak`으로 남고 새 기본값으로 바뀐다(고쳐 둔 값이 있으면 다시 고친다).

## v0.2.1 (2026-10-21, 실기기 v0.2.0 소리·볼륨·소리 끄기 동작 확인 뒤)

- 꽂은 뒤 기다리는 시간 `settle_ms`를 1000 → 200 ms로 줄였다. 기존 `config.ini`에는 `settle_ms=1000`이 적혀 있으니 200으로 고치거나 `config.ini`를 지워 새로 만들게 한다. 남는 지연은 이어폰 자체가 꽂을 때 몇 번 다시 연결되기 때문이다(로그의 `re-enumeration`).
- 스피커를 음소거로 끈 동안(`volume_follow=1`)에는 **콘솔 볼륨이 곧 이어폰 볼륨**이다. 리모컨 Vol+/Vol−는 콘솔 볼륨을 한 칸씩 바꾸고(HOME 메뉴 볼륨 슬라이더에 그대로 보임), 이어폰은 콘솔 볼륨을 따라간다(맨 위 칸 = `volume_max_db`, 한 칸 내려갈 때마다 `volume_step_db`). 콘솔 볼륨 버튼으로 바꾸면 시스템 볼륨 창도 뜬다. 리모컨으로 바꿀 때 그 창이 뜨는지는 실기기에서 확인이 필요하다.

## v0.2.0에서 바뀐 것

| 기능 | 동작 |
|---|---|
| 모든 소리를 이어폰으로 | 기본 모드가 `stream`이다. 이어폰을 꽂으면 약 5초 뒤(꽂을 때 몇 번 다시 연결되는 시간) 스위치 소리가 이어폰으로 나온다 |
| 스피커 끄기 | 이어폰이 1초 이상 정상으로 재생되면 콘솔 스피커(독 모드면 TV)를 끈다. 이어폰을 빼거나, 녹음이 3초 이상 끊기거나(그러면 이어폰도 조용하므로), eoaudio가 꺼지거나, 크래시가 나면 바로(또는 다음 시작 때) 원래대로 되돌린다 |
| 녹음 자가 점검 | 스피커를 끄면 녹음까지 조용해질 수 있다(아직 모름). 그래서 처음에는 소리가 날 때 끄고 녹음 레벨을 비교한다. 녹음이 죽으면 되돌리고 다시 확인해, 두 번 확인되면 그 방법은 버리고 다음 방법(음소거 → 볼륨 0 → 스피커 그대로)을 쓴다. 결과는 펌웨어별로 `state.ini`에 기억한다 |
| 다른 출력이 우선 | 3.5 mm 이어폰, 블루투스 오디오, 닌텐도가 인식한 다른 USB-C 헤드셋이 활성 출력이 되면 eoaudio는 물러난다(이어폰 스트림 멈춤, 아무것도 끄지 않음). 스피커/TV로 돌아오면 다시 재생한다 |
| 다른 장치는 건드리지 않음 | Samsung `04e8`이면서 PID가 `a054/a059/a05b`인 장치만 잡는다. PID는 잡기 전에 확인한다. 다른 USB 장치는 로그에 "left alone"으로 적기만 한다 |
| 볼륨 | 이어폰 리모컨 Vol+/Vol−로 3 dB씩(누르고 있으면 반복). 이어폰 자체 볼륨(FU5)을 쓰고, 안 되면 디지털로 줄인다. 마지막 볼륨은 재연결·재부팅 뒤에도 유지된다(`state.ini`) |
| 콘솔 볼륨 따라가기 | 스피커를 "음소거"로 끈 동안에는 콘솔 볼륨 버튼과 HOME 볼륨 슬라이더로도 이어폰 볼륨이 바뀐다(한 칸 = 3 dB) |
| 재생/정지 | 리모컨 가운데 버튼 = 이어폰 소리 끄기/켜기(누를 때만 동작하고, 누르고 있거나 뗄 때는 무시한다). 게임 자체를 멈추는 기능(가상 컨트롤러로 HOME/+ 누르기)은 넣지 않았다. 가상 컨트롤러가 플레이어 배정을 바꿀 수 있어 위험하다고 판단했다 |
| 로그 | 연결/해제(VID:PID와 판단), 확보 경쟁 결과, 재열거, 상태 전환, 버튼과 그 동작, 볼륨/음소거, 스피커 끄기/되돌리기, 활성 출력 변화, 10초마다 상태 한 줄(`health:`). `log_level=debug`면 1초마다 상세 줄과 리모컨 원본 리포트도 남긴다 |

`capture`(녹음만 확인)와 `tone`(시험음) 모드는 진단용으로 그대로 있다.

## 설치

1. `eoaudio-v0.2.0-sd.zip`을 풀어서 나온 `atmosphere` 폴더를 SD 카드 **루트**에 복사한다(DBI MTP로 끌어다 놓으면 기존 폴더와 합쳐진다). 빌드했다면 `switch/eoaudio/sd/` 안의 내용과 같다.
   ```
   atmosphere/contents/420000000E01C100/exefs.nsp
   atmosphere/contents/420000000E01C100/toolbox.json
   atmosphere/contents/420000000E01C100/flags/boot2.flag
   ```
2. 재부팅한다. 부팅 후 약 5초 뒤에 eoaudio가 시작된다.
3. 확인: `sdmc:/switch/eoaudio/logs/`에 `eoaudio_YYYYMMDD_HHMMSS.txt`가 새로 생기면 동작 중이다.

- v0.1.0을 설치했었다면 그대로 덮어쓰면 된다. 예전 `config.ini`(1단계 capture 설정)는 `config.ini.v1.bak`으로 남기고 새 기본값(`mode=stream`)으로 바꾼다.
- eoaudio가 켜져 있는 동안 **eoprobe를 같이 실행하지 않는다.** 둘이 이어폰을 두고 경쟁한다. eoprobe나 닌텐도 드라이버 관찰 시험(PLAN.md §5 C-0)을 할 때는 `mode=off`로 바꾸고 재부팅한다.
- Tesla 메뉴의 ovl-sysmodules가 있으면 "eoaudio (EO-IC100 USB audio)"로 나오고, 재부팅 없이 끄고 켤 수 있다.

## 시험 순서 (v0.2.0)

> 처음에는 이어폰을 귀에서 조금 떼고 시작한다. 시작 볼륨은 −20 dB다.

1. 휴대 모드에서 소리가 계속 나는 게임을 실행한다.
2. 이어폰을 꽂는다. **약 5초 뒤 이어폰에서 소리가 나야 한다.** 처음 한 번은 스피커도 몇 초 같이 나다가, 게임 소리가 들리는 순간 스피커가 꺼진다(녹음 자가 점검). 이때 이어폰 소리가 2~3초씩 두 번 끊겼다가 스피커가 다시 켜지면, 그 방법이 녹음까지 죽여서 되돌린 것이다. 그러면 다음 방법(볼륨 0)으로 한 번 더 같은 점검을 한다. 결과는 기억되므로 다음부터는 점검 없이 바로 적용된다(로그에 모두 남는다).
3. 1~2분 들어 본다: 딸깍거림, 끊김, 잡음, 지연(화면보다 늦게 들리는지).
4. 리모컨 Vol+/Vol−, 가운데 버튼(소리 끄기/켜기)을 눌러 본다. 콘솔의 볼륨 버튼도 눌러 본다.
5. 이어폰을 뺀다. **스피커 소리가 바로 돌아와야 한다.** 다시 꽂으면 이어폰으로 돌아간다. 볼륨은 직전 값이다.
6. (선택) 이어폰을 꽂은 채 3.5 mm 이어폰을 꽂아 본다. 소리가 3.5 mm로 옮겨 가고 EO-IC100은 조용해져야 한다. 빼면 EO-IC100으로 돌아온다.
7. 결과를 보낸다(아래).

**소리가 안 나면:** `config.ini`에서 `mode=tone`으로 바꾸고 재부팅한다. 시험음이 들리면 USB 전송은 정상이고 녹음 쪽 문제다. 그다음 `mode=capture`로 녹음이 되는지 확인한다(아래 진단 모드).

## 로그 회신 방법

1. 문제가 있었던 실행의 로그를 보낸다: `sdmc:/switch/eoaudio/logs/eoaudio_YYYYMMDD_HHMMSS.txt`(실행마다 새 파일, 시각이 이름에 있다). `*_crash.txt`가 있으면 같이 보낸다.
2. 가능하면 `sdmc:/switch/eoaudio/state.ini`도 보낸다(볼륨과 스피커 끄기 판정이 들어 있다).
3. 끊김이나 지연 문제라면 `config.ini`에서 `log_level=debug`로 바꾸고 같은 상황을 다시 만든 뒤 그 로그를 보낸다.
4. 무엇을 했고 무엇을 들었는지, 대략 몇 시 몇 분이었는지 적어 준다.

### 로그 읽는 법

| 줄 | 뜻 |
|---|---|
| `usb: + 04e8:a05b bus 2 dev 21, 5 interface(s) [...] -> EO-IC100: eoaudio claims it` | USB 장치 연결과 eoaudio의 판단. 다른 장치는 `left alone`이다. `usb: -`는 제거다 |
| `dev: +0 ms acquire IF2 (01/02) of 04e8:a05b: OK` | 이어폰 확보 경쟁 결과. `held by another process`가 보이면 시스템이 먼저 잡은 것이다 |
| `dev: re-enumeration #1: ...` | 꽂을 때의 재연결 |
| `dev: state settling -> ready (...)` | 상태 전환과 이유 |
| `dev: remote Vol+: earphone volume -17.0 dB via FU5 (...)` | 버튼과 그 결과. `via digital gain`이면 장치 볼륨 대신 디지털로 줄인 것 |
| `policy: ...` | 스피커 끄기 판단: 점검 시작, 녹음 유지/죽음, 되돌림, 물러남, 콘솔 볼륨 따라가기 |
| `speaker: SetTargetMute on the speaker (...)` / `speaker: undo ...` | 실제로 스피커를 끈 것과 되돌린 것 |
| `audio out: active output speaker, volume 8 ...` | 활성 출력과 콘솔 볼륨 변화 |
| `health: ...` | 10초마다: 장치 상태와 볼륨, 활성 출력, 정책 단계, 녹음(초당 프레임, 레벨), USB(배치, 실패, 재동기화), 지터 버퍼(수위, 드리프트 보정 `slips`, `underruns`) |

## 끄기, 제거, 부팅이 안 될 때

| 하고 싶은 것 | 방법 |
|---|---|
| 잠시 끄기 | `config.ini`에서 `mode=off`로 바꾸고 재부팅한다(또는 ovl-sysmodules에서 끈다) |
| 부팅 때 시작하지 않게 | `atmosphere/contents/420000000E01C100/flags/boot2.flag`를 지운다 |
| 완전히 제거 | `atmosphere/contents/420000000E01C100/` 폴더를 지운다. 설정·상태·로그(`switch/eoaudio/`)는 필요하면 함께 지운다 |
| 기억한 볼륨과 판정 지우기 | `switch/eoaudio/state.ini`를 지운다 |

**스피커가 조용한 채로 남지 않게:** 스피커를 끄기 전에 원래 상태를 `switch/eoaudio/speaker_restore.txt`에 적어 두고, 되돌리면 지운다. 이어폰을 빼면 바로, 크래시가 나면 예외 처리기가 즉시 되돌린다. ovl-sysmodules로 eoaudio를 강제로 끄면 되돌릴 기회가 없으므로, **끄기 전에 이어폰을 먼저 빼는 것이 좋다.** 깜빡했다면 eoaudio를 다시 켜면(또는 재부팅하면) 시작할 때 되돌린다.

**부팅이 안 되거나 오류 화면이 반복될 때:**
1. 안전장치가 있다. 크래시가 나면 콘솔 전체 fatal 대신 eoaudio만 종료되고 `logs/<실행이름>_crash.txt`가 남는다. 시작 후 60초 안에 끝난 실행이 3번 연속이면 eoaudio는 스스로 꺼진 채로 있는다(`switch/eoaudio/bootguard.txt`를 지우면 다시 켜진다). 그래서 먼저 **최대 3번까지 재부팅**해 본다.
2. 그래도 안 되면 SD 카드를 PC에 꽂거나 hekate → Tools → USB Tools → SD Card로 PC에 연결해 `atmosphere/contents/420000000E01C100` 폴더를 지운다.

## config.ini 항목

`sdmc:/switch/eoaudio/config.ini`. 첫 실행 때 기본값으로 만들어지고, 지우면 다시 만들어진다. 고친 뒤 재부팅하거나 ovl-sysmodules로 껐다 켠다.

| 키 | 기본값 | 뜻 |
|---|---|---|
| `mode` | `stream` | `stream` / `capture`(진단) / `tone`(진단) / `off` |
| `log_level` | `info` | `info` 또는 `debug`(1초 상세 줄, 리모컨 원본 리포트, 디스크립터 hex) |
| `health_s` | 10 | 상태 줄 간격(초) |
| `log_keep` | 30 | 남길 로그 파일 수(오래된 것부터 지움, 0 = 지우지 않음) |
| `speaker_mute` | `auto` | 이어폰 재생 중 스피커/TV 끄기: `auto`(음소거 → 볼륨 0 → 그대로) / `mute` / `volume0` / `off` |
| `volume_db` | −20 | 처음 시작 볼륨. 한 번 바꾸면 `state.ini` 값이 우선한다 |
| `volume_step_db` | 3 | 리모컨 한 번, 콘솔 볼륨 한 칸에 바뀌는 양 |
| `volume_min_db`, `volume_max_db` | −50, 0 | 볼륨 범위(장치 범위 −60~0 dB 안에서) |
| `volume_follow` | 1 | 스피커를 음소거로 끈 동안 콘솔 볼륨을 이어폰 볼륨에 반영 |
| `play_button` | `mute` | 가운데 버튼: `mute`(이어폰 소리 끄기/켜기) / `none` |
| `target_ms` | 20 | 지터 버퍼 목표(ms). 끊기면 늘리고, 지연을 줄이려면 줄인다 |
| `rec_frames`, `rec_buffers` | 240, 8 | 녹음 버퍼 길이(240 = 5 ms)와 개수 |
| `mute_test`, `acquire` | 1, 1 | capture 모드 전용: 스피커 음소거 시험, 이어폰 확보 기록 |
| `tone_hz`, `tone_dbfs` | 1000, −30 | tone 모드의 시험음 |
| `pids` | `a054,a059,a05b` | 받아들일 Samsung(04e8) 제품 ID |
| `settle_ms`, `resets` | 1000, 2 | 꽂은 뒤 기다리는 시간, 시스템이 먼저 잡았을 때 소프트웨어 재연결 횟수 |
| `iso_urbs`, `iso_batches`, `iso_lead` | 4, 4, 16 | isoc 전송 설정(곱은 16 이하) |
| `start_delay_s` | 5 | 부팅 후 시작까지 기다리는 시간 |

잘못된 값은 무시되거나 범위 안으로 맞춰지고, 로그 첫머리에 `config: line N: ...`으로 남는다.

## 진단 모드

- **`mode=capture`**: 이어폰으로 소리를 내지 않고 녹음만 기록한다. 초당 레벨과 버퍼 타이밍, 게임 ID를 남긴다. 게임 소리가 10초 이어지면 부팅당 한 번 스피커를 3가지 방법(음소거, 볼륨 0, 마스터 볼륨 0)으로 4초씩 꺼 보고, 녹음이 유지되는지 `mute test: done` 표로 남긴다. "capture kept"로 나온 방법은 `state.ini`에 기록되어, stream 모드가 다시 점검하지 않고 바로 쓴다.
- **`mode=tone`**: 녹음 없이 1 kHz 시험음을 이어폰으로 보낸다. 볼륨과 가운데 버튼도 동작한다.

## 동작 방식

- **장치 확보**: VID 04e8 InterfaceAvailableEvent를 기다리다가 원하는 PID의 인터페이스가 보이면 스트리밍 → AudioControl → HID 순서로 전부 acquire한다. USB 이벤트 뒤 10초 동안은 2 ms마다 QueryAvailable도 확인한다. 재열거로 세션이 사라지면 새 인스턴스를 다시 잡는다. `settle_ms` 동안 변화가 없으면 설정한다. 재생 인터페이스를 시스템이 먼저 잡았으면 `ResetDevice`로 다시 연결시켜 경쟁한다(`resets`회). 연결된 모든 USB 장치는 2초마다 목록으로 비교해 로그에 남긴다(QueryAll, 확보하지 않음).
- **설정과 전송**: 48 kHz/16-bit/스테레오 재생 alt(IF2 alt1, EP 0x03, 192 B/ms), SET_CUR 48000, FU5 볼륨/음소거. `usbHsEpBatchBufferAsync(unk1 = 0, unk2 = 절대 시작 프레임)`, 4 urb × 4 배치, `0xD28C`면 재동기화. 스트리밍 스레드는 SD에 쓰지 않는다(로그는 RAM 링에 쌓고, 우선순위가 낮은 스레드가 0.5초마다 쓴다).
- **지터 버퍼와 드리프트 보정**: 링에 20 ms가 차면 재생을 시작한다. 평균 수위가 목표에서 녹음 버퍼의 1/4(1.25 ms) 이상 벗어나면 4 ms 블록 하나를 193/191 → 192 프레임으로 선형 보간해 한 샘플을 빼거나 넣는다. 평소에는 최대 100 ms에 한 번, 5 ms 이상 벗어나면 블록마다 보정한다.
- **스피커 정책** (`source/policy.c`, 호스트 테스트 `tests/test_policy.c`): 250 ms마다 audctl 활성 출력/볼륨/음소거를 읽고, 1초마다 녹음 레벨을 받는다. 확인되지 않은 방법은 녹음 레벨이 −50 dBFS보다 클 때만 적용한다. 적용 뒤 녹음이 디지털 무음이나 −75 dBFS 아래로 떨어지면 되돌리고, 녹음이 돌아오는 것이 두 번 확인되면 그 방법을 버린다. 되돌린 뒤에도 조용하면 "게임이 조용했던 순간"으로 보고 나중에 다시 시도한다(3번까지). 시스템이 음소거를 풀면 세션당 3번까지 다시 건다. 녹음기가 3초 동안 아무 프레임도 주지 않으면(그러면 이어폰도 조용하다) 기억된 방법이라도 바로 되돌리고, 녹음이 다시 들어올 때까지 아무것도 끄지 않는다.
- **스레드**: 모두 코어 3. isoc 0x24, 녹음 0x26, 메인(장치 경쟁, 정책) 0x28, 리모컨 0x2E, 로그 0x3B. 메모리는 약 1 MB다.

## 빌드와 테스트

devkitPro switch-dev(libnx, devkitA64 GCC 15.2로 확인). 이 PC에서는 WSL Ubuntu의 `/opt/devkitpro`를 쓴다.
```bash
make -C switch/eoaudio
```
`switch/eoaudio/eoaudio.nsp`가 만들어지고, 같은 내용이 SD 배치 그대로 `switch/eoaudio/sd/`에 놓인다. npdmtool이 출력하는 `Failed to get ... (field not present)` 줄은 JSON에 없는 선택 항목에 대한 안내다. devkitPro sysmodule 템플릿에서도 똑같이 나온다.

호스트 단위 테스트(PCM 링, 지터 버퍼/드리프트 시뮬레이션, 디지털 볼륨, 설정 파서, 사용자 a05b 디스크립터, 스피커 정책 12개 시나리오, state.ini):
```bash
make -C switch/eoaudio/tests check
```

## 확인이 필요한 것과 한계

- FinalOutputRecorder가 sysmodule에서 열리는지, 게임 소리를 담는지, 스피커를 꺼도 녹음이 유지되는지는 실기기에서 처음 확인된다. 녹음이 안 되면 B는 보류하고 접근 C(닌텐도 드라이버가 이 이어폰을 받아들이게 하는 MITM)로 간다.
- 콘솔 볼륨 따라가기는 "녹음이 스피커 볼륨의 영향을 받지 않는다"는 가정 위에 있다. 콘솔 볼륨을 바꿀 때 이어폰 볼륨이 두 배로 크게 바뀌는 것 같으면 `volume_follow=0`으로 두고 알려 준다. 볼륨 0 방식으로 스피커를 끈 경우에는 콘솔 볼륨을 누르면 스피커가 다시 0으로 돌아가고, 이어폰 볼륨은 리모컨으로만 바뀐다.
- 가운데 버튼 리포트(`01 04 00`)는 아직 실기기에서 보지 못했다. 다르게 오면 `log_level=debug` 로그의 `remote: report`로 알 수 있다.
- 지연은 대략 녹음기 내부 지연 + 5 ms(녹음 버퍼) + 20 ms(지터 버퍼) + 16 ms(USB 대기열)로 예상한다.
- 녹화를 막는 게임은 무음일 수 있다. 시스템 업데이트로 audrec/usb:hs/audctl 동작이 바뀌면 다시 확인해야 한다. 펌웨어가 바뀌면 `state.ini`의 스피커 판정은 자동으로 잊는다.
