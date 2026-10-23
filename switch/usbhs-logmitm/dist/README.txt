EO-IC100 USB-C 이어폰 × 닌텐도 스위치 — 2026-10-23 묶음
=======================================================

삼성 USB-C 이어폰 EO-IC100을 스위치에서 쓰기 위한 세 가지를 담았습니다.

  1. USB 오디오 드라이버  usbhs-logmitm 0.2.1 (Atmosphère sysmodule)
     스위치가 이어폰을 "USB 스피커"로 인식합니다. 게임, 홈 화면, 홈브루 소리가
     모두 이어폰으로 나오고 콘솔 볼륨도 그대로 동작합니다.
  2. FM 라디오 앱         eofm 0.2.2 (hbmenu 홈브루)
     이어폰에 내장된 FM 라디오를 켜고, 채널을 찾고, 즐겨찾기를 관리하는 한글 앱입니다.
  3. 라디오 오버레이      eofm-ovl 0.1.1 (Ultrahand / Tesla 오버레이)
     게임 중에 라디오 전원, 볼륨, 주파수, 저장한 방송을 조절합니다.

셋은 따로 써도 됩니다. 라디오(2, 3)는 드라이버(1)가 없어도 동작합니다.


확인된 환경
-----------
- HOS 22.1.0 + Atmosphère 1.11.1, EO-IC100 (USB ID 04e8:a05b)
- 04e8:a054, 04e8:a059 변형은 같은 규칙이 적용되지만 실기기에서 확인하지 않았습니다.
- 확인 안 된 것: 이어폰 리모컨 버튼, 마이크, 슬립 후 복귀, TV 모드.


설치
----
이 zip 안의 atmosphere, switch 폴더를 SD 카드 루트에 복사합니다
(DBI MTP 또는 Hekate UMS). 필요한 것만 골라 넣어도 됩니다.

  [1 드라이버]
    atmosphere/contents/420000000E01C0C0/exefs.nsp
    atmosphere/contents/420000000E01C0C0/mitm.lst
    atmosphere/contents/420000000E01C0C0/flags/boot2.flag
    switch/usbhs-logmitm/config.ini
  [2 라디오 앱]
    switch/eofm/eofm.nro
  [3 오버레이] Ultrahand(또는 Tesla Menu)와 nx-ovlloader가 깔려 있어야 합니다.
    switch/.overlays/eofm-ovl.ovl

드라이버를 넣었으면 재부팅합니다. 이어폰을 꽂으면 볼륨 아이콘이 USB 스피커로 바뀌고
소리가 이어폰으로 나옵니다. 처음에는 이어폰을 귀에서 조금 떼고 확인하세요.
이어폰을 같은 방식으로 다루는 다른 sysmodule(예: eoaudio, 420000000E01C100)이 있으면
먼저 지우세요.

드라이버는 Ultrahand에서 켜고 끌 필요가 없습니다. 부팅 때 자동으로 시작합니다.
실행 중에 이 프로세스를 강제로 끄지 마세요(콘솔 오류가 날 수 있습니다).


라디오 쓰는 법
--------------
- 이어폰 줄이 안테나입니다. 줄을 펴면 수신이 좋아집니다.
- 이어폰을 먼저 꽂고 몇 초 뒤 hbmenu에서 eofm을 실행합니다.
- X(채널 찾기)로 87.5~108 MHz를 훑어 방송을 찾습니다(약 20초). 찾은 목록은 저장됩니다.
- A 전원, ← → 0.1 MHz, L R 1 MHz, ZL ZR 이전/다음 방송, ↑ ↓ 볼륨, Y 즐겨찾기.
- + 로 나가면 라디오는 이어폰에서 계속 나옵니다(게임하면서 듣기). B는 끄고 나가기.
- 게임 중에는 L + ↓ + R3로 Ultrahand를 열고 "EO Radio"에서 조절합니다.
  오버레이는 라디오 앱과 같은 설정(찾은 채널, 즐겨찾기)을 씁니다.
  라디오 앱이 실행 중일 때는 오버레이가 "라디오 앱이 사용 중"으로 표시됩니다.
- 이어폰을 뽑으면 라디오가 꺼집니다. 다시 꽂은 뒤에는 앱이나 오버레이에서 켜 주세요.
- 라디오 소리는 이어폰 안에서 나며 본체 스피커로는 들을 수 없습니다.


끄기 / 제거
-----------
- 드라이버 끄기: atmosphere/contents/420000000E01C0C0/flags/boot2.flag 를 지우고 재부팅
- 드라이버 제거: atmosphere/contents/420000000E01C0C0 폴더와 switch/usbhs-logmitm 폴더를
  지우고 재부팅
- 부팅이 멈추면: 전원 버튼 12초로 끄고, Hekate → Tools → USB Tools → SD Card(UMS)로
  PC에 연결해 atmosphere/contents/420000000E01C0C0 폴더를 지운 뒤 재부팅합니다.
- 라디오: switch/eofm 폴더와 switch/.overlays/eofm-ovl.ovl 을 지웁니다.


로그 (문제가 있을 때 보내 주세요)
---------------------------------
- 드라이버: switch/usbhs-logmitm/logs/ (실행마다 새 파일, 최근 20개)
- 라디오 앱: switch/eofm/logs/eofm_*.txt
- 오버레이: switch/eofm/logs/eofm-ovl.txt

소스와 자세한 설명: https://github.com/2seunghee/eo-ic100-switch
