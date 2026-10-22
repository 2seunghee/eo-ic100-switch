EO-IC100 USB-C 이어폰 → 닌텐도 스위치 USB 오디오 (usbhs-logmitm v0.2.1)
======================================================================

삼성 USB-C 이어폰 EO-IC100을 스위치가 "USB 스피커"로 인식하게 해 주는
Atmosphère sysmodule입니다. 스위치 자체 오디오 출력을 쓰므로 게임, 홈 화면,
홈브루 소리가 모두 이어폰으로 나오고, 콘솔 볼륨도 그대로 동작합니다.

동작 원리: 스위치 오디오 드라이버가 이어폰의 USB 설정 디스크립터를 읽는 순간,
출력/마이크 단자 종류(0x0402 Headset)를 0x0302 Headphones / 0x0201 Microphone으로
고쳐 씁니다(3바이트). 이어폰 자체에는 아무것도 쓰지 않습니다. 뽑으면 원래대로입니다.


확인된 환경
-----------
- HOS 22.1.0 + Atmosphère, EO-IC100 (USB ID 04e8:a05b)
- 04e8:a054, 04e8:a059 변형은 같은 규칙이 적용되지만 실기기에서 확인하지 않았습니다.
- 확인 안 된 것: 리모컨 버튼, 마이크, 슬립 후 복귀, TV 모드.


설치
----
1. 이 zip 안의 atmosphere, switch 폴더를 SD 카드 루트에 복사합니다
   (DBI MTP 또는 Hekate UMS).
     atmosphere/contents/420000000E01C0C0/exefs.nsp
     atmosphere/contents/420000000E01C0C0/mitm.lst
     atmosphere/contents/420000000E01C0C0/flags/boot2.flag
     switch/usbhs-logmitm/config.ini
2. 이어폰을 같은 방식으로 다루는 다른 sysmodule(예: eoaudio, 420000000E01C100)이
   있으면 먼저 지우거나 flags/boot2.flag를 지웁니다. 둘이 동시에 이어폰을 잡으려 합니다.
3. 재부팅합니다.
4. 이어폰을 꽂으면 볼륨 아이콘이 USB 스피커로 바뀌고 소리가 이어폰으로 나옵니다.
   처음에는 이어폰을 귀에서 조금 떼고 확인하세요.

Ultrahand 등에서 켜고 끌 필요가 없습니다. 부팅 때 자동으로 시작합니다.
실행 중에 이 프로세스를 강제로 끄지 마세요(콘솔 오류가 날 수 있습니다).


끄기 / 제거
-----------
- 끄기: atmosphere/contents/420000000E01C0C0/flags/boot2.flag 를 지우고 재부팅
- 제거: atmosphere/contents/420000000E01C0C0 폴더와 switch/usbhs-logmitm 폴더를 지우고 재부팅
- 부팅이 멈추면: 전원 버튼 12초로 끄고, Hekate → Tools → USB Tools → SD Card(UMS)로
  PC에 연결해 atmosphere/contents/420000000E01C0C0 폴더를 지운 뒤 재부팅합니다.


설정 (switch/usbhs-logmitm/config.ini, 바꾼 뒤 재부팅)
-----------------------------------------------------
- rewrite=terminal   디스크립터 고쳐 쓰기(해결책). none이면 고치지 않고 기록만 합니다.
- programs=audio     스위치 오디오 드라이버만 가로챕니다.
- read_ctrl_data=0, sync=0   일상용. 문제를 기록해 보낼 때만 1로 바꿉니다.
- 로그: switch/usbhs-logmitm/logs/ (실행마다 새 파일, 최근 20개 보관)


문제가 있을 때
--------------
read_ctrl_data=1, sync=1로 바꾸고 재부팅한 뒤, 증상을 재현하고
switch/usbhs-logmitm/logs/ 의 최신 로그를 보내 주세요.
