# pid.codes VID 0x1209：已分配 / 未分配 PID 清单

来源：https://pid.codes/1209/（抓取日期 2026-10-19）

- 已分配 PID：**1005** 个
- 未分配 PID：**64531** 个，合并为 **708** 个连续区间

> 注意：pid.codes 的 PID 是按申请分配的。下面列的是当前页面上的未分配值；
> 正式分发前请通过 pid.codes 申请（开源/爱好项目免费），否则可能被分配给别人。

## 当前项目选择

本项目已选定 **`0x1209:0xD1CE`**（hexspeak：`D1CE` = "DICE"）作为 XUSB persona 的 PID。
更早的候选 `0x5D01` **已不再使用**。下面的推荐候选表与未分配区间清单是抓取时的
分配快照，未随本次选择修改；代码中的实际取值以 `src/usb_xinput.h` 为准。

## 推荐候选（抓取时的未分配值）



| PID | 说明 |
|---|---|
| `0xD1CE` | **已选定**：hexspeak "DICE"，pid.codes `0x1209:0xD1CE` |
| `0x5D01` | （已弃用，早期候选）XUSB 子类 0x5D + 协议 0x01 |
| `0x6160` | BL616 芯片家族，好记 |
| `0x3601` | Xbox 360 联想，好记 |
| `0x3600` | 同上 |

当前代码使用 **0x1209:0xD1CE**；换 PID 只需改 `USB_XINPUT_PID`（以及描述符自检的期望值）。

## 未分配 PID 区间（完整）

```
0000
0011-006F
0071-01BF
01C1-01CA
01CC-0255
0257-0513
0515-0539
053B-0BAA
0BAC-0CBC
0CBE-0D31
0D33-1000
1008-1775
1777-1985
1987-1AB4
1AB7-1FFF
200B-200F
2013
201B-201F
2029-202F
2032-203F
2043
2046-2047
2049-204F
2053-205F
2061-2068
206A-206F
2071-2076
2078-208F
2091-20BE
20C0-20FF
2102-2118
211A-2126
2128-2136
2138-214B
214E-214F
2152-216F
2171-21F6
21F8-21FF
2202
2204-2210
2212-2221
2223-2230
2232-22FF
2306-2322
2324-2326
2329-2332
2338-2341
2349-234F
2351-2356
2359-23FF
2401
2403
2405-241D
241F
2421-2423
2425-243F
2441
2443
2446-2467
2469-2487
2489-24FF
2501-2504
2506-2524
2526-2551
2553-256E
2570-25EF
25F1-25FF
2601-2608
260A-2629
262B-2634
2636-263F
2641-26FF
2701
2703-272F
2731-2776
277A-27FF
2802-2811
2813-2827
2829-282F
2831
2833-2881
2884-28FF
2901-2924
2926-2981
2984-29AE
29B1-29FF
2A02-2AED
2AF2
2AF8-2AFF
2B01-2BAC
2BAE-2C92
2C94-2E7E
2E81-2F06
2F08-2FA1
2FA3-2FFF
3001-3002
3004-301F
3021-302F
3031-306F
3071-30FF
3101-3140
3143-316C
316E-317D
317F-320F
3211-3248
324A-3251
3254-326F
3271-32B0
32B2-3303
3307-331F
3322-3332
3334-3335
3337-3374
3376-3433
3435-3441
3444-345A
345D-34FF
3502-3612
3614-368F
3691-3700
3702-3705
3707-3738
373A-3756
3758-3844
3846-3918
391B-392E
3930-3932
3934-3938
393A-39FF
3A01-3DBC
3DBE-3EFF
3F01-3FFF
4001-4003
4005-403F
4041-4095
4097-4120
4123-4125
4127-414B
414E-4152
4154-41FF
4204-4222
4224-4236
4238-4241
4243-4251
4253-4255
4257-4268
426A-429F
42A2-4320
4323-4352
4354-4355
4357-43AB
43AD-4442
4447-4512
4514-451F
4521-4544
4546-4559
455B-4566
4568-4572
4574-4645
4647-4648
464A-464F
4656-4664
4666-4703
4705-4746
4749-474F
4751-4756
4758-4800
4802
4804-484B
484D-484F
4851-4852
4854-4883
4885-4918
491A-491F
4922-4A00
4A02-4A45
4A47-4A4E
4A50-4A61
4A63-4B41
4B43-4B4A
4B4C-4B55
4B57-4C47
4C49-4C52
4C54-4C5F
4C62-4D00
4D02-4D40
4D46-4D48
4D4A-4D4C
4D4E-4D52
4D54-4D57
4D59-4D68
4D6A-4DDC
4DE0-4DEF
4DF3-4DF5
4DF7-4DFF
4E01-4E41
4E43-4E4D
4E4F-4E55
4E57-4EB0
4EB2-4F44
4F46-4F49
4F4D-4F53
4F55-4FB2
4FB4-4FFF
5001-501F
5021-5037
503A-504F
5051-5053
5055-506F
5071-50AF
50B1-50D9
50DB-5113
5115-514F
5151-5152
5154-5169
516D-5221
5223-5241
5243-5251
5253-5283
5285-5303
5306-534F
5351-5384
5386-5388
538A-539F
53A1-53BF
53C2-5431
5433-5453
5455-5456
5458-5500
5506-554C
554F-5554
5556-557C
557E-5686
5688-569B
569D-570E
5710-5715
5717-571B
571D-573B
573D-573F
5742-574F
5751-57FF
5802-5857
5859-5954
5956-5959
595B-5A0B
5A0D-5A21
5A23-5A51
5A53-5A59
5A5B-5AB0
5AB3-5AEF
5AF2-5AFD
5AFF-5BEF
5BF1-5BFF
5C01-5C50
5C52-5E7B
5E7D-5EFF
5F01-5FFF
6001-600C
600E-6035
6037-604F
6051-605F
6061-6101
6103-610F
6111-6261
6263-6361
6363-6368
636A-636F
6371-63C0
63C2-63FF
6401-6403
6405-6463
6465-6501
6503-656F
6571-665F
6661-6665
6668-6686
6688-6741
6743-6788
678A-67FF
6801-6803
6805-6808
680A-686F
6871-689F
68A1-6938
693A-6947
694A-6968
696A-696F
6972-69A0
69A2-6ABA
6ABC-6BCE
6BD0-6CFF
6D0A-6DFF
6E03-6E6D
6E6F-6EFF
6F01-6F77
6F7A-6FFF
7002-7041
7043-704F
7051-706F
7071-70B0
70B2-7101
7104-710F
7112-714F
7151-71C3
71C6-71FF
7201-722F
7231-7271
7273
7275-7330
7333-7365
7367-736F
7372-737F
7383-7387
7389-73E9
73EC-7400
7403-740F
7411-7521
7523-752F
7532-7550
7552-75FF
7601-7664
7666-7671
7673-7688
768A-768F
7694-76FF
7703-7776
7779-7786
7788-77FF
7801-7876
7878-7887
7889-7A00
7A02-7A3A
7A3C-7A9F
7AA1-7BCF
7BD1-7C87
7C89-7CFF
7D03-7E79
7E7B-7EAC
7EAE-7FFF
8001-8010
8012-8047
8049-804F
8052-8059
805B-807F
8081-8085
8088-809F
80A1-80FF
8102-8117
8119-8122
8124-8129
812B-8139
813C-8190
8193-8241
8244-824F
8252-82FF
8301-8471
8473-8660
8663-8737
8739-8776
8778-8786
8789-87FF
8802-8815
8817-8843
8845-8850
8852-8884
888A-88BE
88C0-88E9
88EC-8988
898A-898F
8992-8AFF
8B01-8B16
8B18-8CAD
8CAF-8FFF
9002-9020
9022-908F
9091-9100
9103-911F
9122-91FF
9201-9208
920A-920F
9212-9228
922A-9316
9318-9695
9697-9700
9702-9778
977A-9800
9802-9997
999A-9DB4
9DB6-9FFF
A004-A006
A009-A032
A034-A0D0
A0D2-A0FF
A101-A106
A108-A10B
A10D-A118
A11A-A164
A166-A16F
A171-A181
A183-A1E4
A1E6-A1F0
A1F2-A1FF
A201-A2B1
A2B3-A31F
A321-A37F
A381-A3A3
A3A6-A41F
A421-A454
A456-A515
A517-A559
A55B-A5B2
A5B4-A601
A603-A699
A69B-A6E0
A6E2-A710
A712-A76F
A771-A7E9
A7EB-A7FF
A801-A804
A806-A830
A832-A9FF
AA01-AA0A
AA0C-AA60
AA63-AA87
AA89-AAA9
AAAB-AB0B
AB0D-AB3C
AB3E-ABAC
ABAE-ABB7
ABBB-ABBF
ABC2-ABCC
ABCE-ABD0
ABD2-ABEC
ABEF-ABFE
AC00-AC1C
AC1E-ACAA
ACAC-ACC0
ACC2-ACCF
ACD1-ACDB
ACDD-ACDF
ACE2-ACE4
ACE6-ACEA
ACEC
ACEF-AD0D
AD0F
AD11-ADA0
ADA3-ADAF
ADB1-ADD9
ADDB-ADEF
ADF1-AE00
AE02-AE6D
AE6F-AE85
AE87-AF00
AF02-AFFF
B002
B004-B006
B00A
B00C-B00F
B012-B01C
B01E-B057
B059-B0AF
B0B1-B0DD
B0DF-B0FF
B101-B125
B127-B181
B183-B194
B197-B1BF
B1C2-B20F
B211-B2A7
B2A9-B41F
B421-B746
B748-B802
B804-B805
B807-B9FF
BA02-BA54
BA56-BAAF
BABC-BABD
BABF-BAD0
BAD3-BAD9
BADC-BADD
BADF-BAFF
BB04
BB06
BB08-BB22
BB24-BBB1
BBB3-BBBA
BBBC-BBCF
BBD3-BBED
BBEF-BC00
BC02-BCBB
BCBD-BCDD
BCDF-BCFF
BD01-BD05
BD07-BD0F
BD11-BDB0
BDB2-BE79
BE7B-BEB9
BEBB-BED9
BEDB-BEE4
BEE6-BEEA
BEEC-BEED
BEF0-BFFF
C002
C004-C00A
C00E-C010
C012-C034
C036-C050
C052-C0BF
C0C2-C0C9
C0CB-C0D0
C0D2
C0D4-C0D9
C0DB-C0DD
C0DF
C0E1-C0F4
C0F6-C0FD
C0FF-C10B
C10D-C12B
C12D-C1A9
C1AB-C1B0
C1B2-C1C0
C1C2-C310
C312-C54F
C551-C559
C55B
C55F-C64A
C64C-C67F
C681-C6BD
C6BF-C89F
C8A3-C8C7
C8C9-C9C8
C9CA-CA00
CA03-CA1B
CA1E-CA1F
CA21-CA6F
CA72-CAC0
CAC4-CACD
CACF-CADD
CADF-CAE9
CAEB-CAFD
CAFF-CB00
CB03-CB0A
CB0C-CB64
CB66-CB73
CB75-CBFF
CC02-CC13
CC15-CC1C
CC1E-CC3C
CC3E-CC85
CC88-CCA9
CCAB-CCCB
CCCD-CD06
CD08-CDEE
CDF0-CE92
CE94-CEAF
CEB1-CECD
CECF-CEDD
CEE1-CF1F
CF21-D002
D004-D00C
D00E-D016
D018-D034
D036-D066
D068-D089
D08B-D10C
D10E
D111-D11C
D11E-D186
D188-D1A5
D1A7-D1B4
D1B7-D35B
D35D-D368
D36A-D3D7
D3D9-D4FF
D501-D665
D667-D705
D707-D9CF
D9D1-DA14
DA16-DA41
DA43-DA79
DA7B-DA9F
DAA1-DAC9
DACB-DAD9
DADB-DAFF
DB01-DB41
DB43-DB4F
DB51-DC20
DC22-DCAF
DCB2-DD82
DD84-DDA9
DDAB-DDDC
DDDE-DE19
DE1B-DE1F
DE21-DE5B
DE5D-DEAC
DEAE
DEB1-DEC0
DEC2-DEC9
DECB-DED0
DED2-DED9
DEDB-DEEC
DEEE-DEEF
DEF1-DEFF
DF04-DF10
DF12-DFA8
DFAD-DFFF
E002-E003
E005-E006
E008-E07D
E07F-E115
E117-E119
E11B-E127
E129-E1E0
E1E2-E1EB
E1ED-E3CD
E3CF-E3E0
E3E4-E4ED
E4EF-E4FF
E503-E581
E583-E666
E66A-E738
E73A-EA68
EA6A-EACB
EACE-EAE9
EAEC-EB00
EB02-EBA6
EBA8-EBEA
EBEC-EBFF
EC01
EC03-EC19
EC1B-EDA0
EDA4-EDEC
EDEE-EDFF
EE01
EE04-EE2B
EE2D-EEA0
EEA2-EEED
EEEF-EEF0
EEF2-EEFF
EF01-EF0B
EF0D-EFEE
EFF0-EFF9
EFFB-EFFD
EFFF
F001-F00C
F00E
F010-F042
F044-F0C6
F0C8-F0D9
F0DB-F0EF
F0F1-F102
F104-F10A
F10C-F122
F124-F12D
F12F-F14F
F151-F169
F16D-F1CF
F1D1
F1D3-F1DD
F1DF-F1EF
F1F1-F208
F20A-F313
F315-F37F
F381-F3FB
F3FD-F47F
F481-F499
F49B-F4F3
F4F5-F4FF
F503-F53F
F543-F5FF
F601-F665
F667-F700
F702-F722
F724-F781
F783-F7FF
F801-F9FF
FA01-FA10
FA12-FA44
FA46-FA47
FA49
FA4C-FA54
FA56
FA59-FA6F
FA71-FAB0
FAB2-FAC9
FACB-FACD
FACF-FAD0
FAD2-FAD9
FADB-FADD
FADF-FAEF
FAF1-FB9F
FBA1-FBFF
FC01-FC2F
FC31-FD41
FD43-FDFC
FDFE-FE04
FE08-FE7F
FE81-FEE0
FEE2-FEEC
FEEE-FEFD
FEFF-FF00
FF0B-FF3F
FF41-FF4F
FF51-FF5F
FF61-FF8F
FF92-FFAF
FFB1-FFC9
FFCB-FFFD
```

## 已分配 PID（按值排序）

| PID | 设备 |
|---|---|
| `0x0001` | pid.codes Test PID |
| `0x0002` | pid.codes Test PID |
| `0x0003` | pid.codes Test PID |
| `0x0004` | pid.codes Test PID |
| `0x0005` | pid.codes Test PID |
| `0x0006` | pid.codes Test PID |
| `0x0007` | pid.codes Test PID |
| `0x0008` | pid.codes Test PID |
| `0x0009` | pid.codes Test PID |
| `0x000A` | pid.codes Test PID |
| `0x000B` | pid.codes Test PID |
| `0x000C` | pid.codes Test PID |
| `0x000D` | pid.codes Test PID |
| `0x000E` | pid.codes Test PID |
| `0x000F` | pid.codes Test PID |
| `0x0010` | pid.codes Test PID |
| `0x0070` | Nerves Project Ethernet + Serial Gadget |
| `0x01C0` | Input Club Kiibohd Device |
| `0x01CB` | Input Club Kiibohd Device Bootloader |
| `0x0256` | Schwalm & Tate LLC pISO Raspberry Pi Hat |
| `0x0514` | Lotlab Lot60 BLE Keyboard |
| `0x053A` | Hackerspace San Salvador HSSV SAMR21-Mote |
| `0x0BAB` | Bitgamma OÜ Palette |
| `0x0CBD` | Andrzej Szombierski kuku.eu.org keyboard |
| `0x0D32` | ODrive Robotics ODrive |
| `0x1001` | InterBiometrics USB Hub |
| `0x1002` | InterBiometrics USB Relais |
| `0x1003` | InterBiometrics IBSecureCam-P |
| `0x1004` | InterBiometrics IBSecureCam-O |
| `0x1005` | InterBiometrics IBSecureCam-N |
| `0x1006` | InterBiometrics Mini IO-Board |
| `0x1007` | e-radionica.com Croduino SAMD |
| `0x1776` | System76 System76 Thelio Io |
| `0x1986` | dgrubb Jaguar USB Tap |
| `0x1AB5` | Arachnid Labs Tsunami |
| `0x1AB6` | Arachnid Labs Tsunami Bootloader |
| `0x2000` | Zygmunt Krynicki Lantern Brightness Sensor |
| `0x2001` | OSHEC Pi-pilot opensource and openhardware autopilot system |
| `0x2002` | Peter Lawrence PIC16F1-USB-DFU-Bootloader |
| `0x2003` | Peter Lawrence SAMDx1-USB-DFU-Bootloader |
| `0x2004` | GCBASIC GCBASIC Serial CDC Stack |
| `0x2005` | GCBASIC GCBASIC OakTree Stack |
| `0x2006` | GCBASIC GCBASIC Simulation Stack |
| `0x2007` | MultiBus MultiBus Bridge |
| `0x2008` | picolume PicoLume Receiver |
| `0x2009` | picolume PicoLume Remote |
| `0x200A` | YARD2 PIC16F1-USB-CDC-Bootloader |
| `0x2010` | Ravikiran Bukkasagara OpenLink-V1 CMSIS-DAP |
| `0x2011` | JS-SimSport Exapandable Dash |
| `0x2012` | Positron Electronic 12pad |
| `0x2014` | cool tech zone Tangara |
| `0x2015` | UNGsoftware The GRUB Switch |
| `0x2016` | Cupkee Cupkee |
| `0x2017` | Benjamin Shockley Mini SAM M4 |
| `0x2018` | EEZ STM32 MCU board |
| `0x2019` | Jonas Persson PunchBOOT |
| `0x201A` | GregoryGHamilton CW USB Adapter |
| `0x2020` | Captain Credible Gate Crystal |
| `0x2021` | Riley Rainey AGC DSKY Alarm Panel Replica |
| `0x2022` | Riley Rainey AGC DSKY Keyboard Replica |
| `0x2023` | SSIS Robotics T-Display rp2040 |
| `0x2024` | AtrivaTECH PicUNO RP2040 |
| `0x2025` | OpenEnergyMonitor emon |
| `0x2026` | Riley Rainey Tempo SAMD51-based Logger |
| `0x2027` | rppicomidi twist4 |
| `0x2028` | Zeyback ZeyBack R2RDAC |
| `0x2030` | Czech maker Maker badge |
| `0x2031` | Czech maker ES3ink |
| `0x2040` | mikepython RP2040smol |
| `0x2041` | Matthew Via viaems |
| `0x2042` | WLAN Pi USB Ethernet |
| `0x2044` | Kenny Black Printing LLC KBP Macro Pad |
| `0x2045` | EEZ CM5 STM32 H7 module |
| `0x2048` | Housedillon.com MRF49XA Transciever |
| `0x2050` | myStorm Alloy |
| `0x2051` | myStorm Amalgam |
| `0x2052` | myStorm BlackIce V |
| `0x2060` | OM5XM GdmIface |
| `0x2069` | TinLethax iCEBlaster |
| `0x2070` | Vegetronix VG3D Printer |
| `0x2077` | Smuggr LoRaptor |
| `0x2090` | Pawpaw Works MIDIval Renaissance |
| `0x20BF` | Butterfly OSKey |
| `0x2100` | TinyFPGA TinyFPGA B1 and B2 Boards |
| `0x2101` | TinyFPGA TinyFPGA A-Series Programmer |
| `0x2119` | Firmament-Autopilot team Firmament USB driver |
| `0x2127` | TreasureCoastDesigns TR-Cowbell |
| `0x2137` | Warsaw Hackerspace Web I2C Programmer |
| `0x214C` | Josh Gao Passing Link |
| `0x214D` | Josh Gao Passing Link Bootloader |
| `0x2150` | Oddly Specific Objects Sensor Watch UF2 Bootloader |
| `0x2151` | Oddly Specific Objects Sensor Watch |
| `0x2170` | OpenThermalCamera OpenThermalCamera |
| `0x21F7` | Keycard Shell |
| `0x2200` | Dygma Raise Bootloader |
| `0x2201` | Dygma Raise Keyboard |
| `0x2203` | its_kronos The Breadboard+ |
| `0x2211` | Summa Cogni Mathpad |
| `0x2222` | LabConnect Signalgenerator |
| `0x2231` | Nullstalgia NullWiiCon |
| `0x2300` | Keyboardio Model 01 Bootloader |
| `0x2301` | Keyboardio Model 01 |
| `0x2302` | Keyboardio Atreus 2 Bootloader |
| `0x2303` | Keyboardio Atreus 2 |
| `0x2304` | ErgoKB Phoenix Board |
| `0x2305` | cnlohr USBHS 32x WS2812B driver |
| `0x2323` | bytewerk.org candleLight |
| `0x2327` | K.T.E.C. ⌧ Bootloader Device |
| `0x2328` | K.T.E.C. ⌨ Keyboard Device |
| `0x2333` | Kai Ryu Kimera |
| `0x2334` | Kai Ryu Staryu |
| `0x2335` | Portwell Sense8 |
| `0x2336` | Portwell Sense8 |
| `0x2337` | /Dev /Net |
| `0x2342` | Andreas Bogk Big Red Button |
| `0x2343` | jaseg bluefnorf |
| `0x2344` | jaseg MoaRGB |
| `0x2345` | VV-Soft Simple Generic USB HID IO |
| `0x2346` | Phobos Instruments QUIK Mini |
| `0x2347` | Domesday86 Domesday-Duplicator |
| `0x2348` | Till&apos;s USB Gadgets ScOpen - A Low-Cost Scope |
| `0x2350` | CircuitArt CircuitArt RP2350 zero |
| `0x2357` | KarolKucza TinyPassword |
| `0x2358` | ErgoGain ErgoGain |
| `0x2400` | phooky Snap-Pad |
| `0x2402` | kien242 Kimiro nRF52840 |
| `0x2404` | Badgerd Technologies B.V. Badgerd SDWire |
| `0x241E` | tcsenpai EMPHarvest |
| `0x2420` | HappyShooting Community Schwurbler |
| `0x2424` | Thunkit Electronics T85DUINO |
| `0x2440` | GnuPG e.V. An OpenPGP Token |
| `0x2442` | Harry Gill Opilio - PC Fan/Pump Controller v1 |
| `0x2444` | Harry Gill RTIC DFU Bootloader |
| `0x2445` | Openrobots tk3-paparazzi |
| `0x2468` | My DIY projects Hi-Resolution Audio Interface |
| `0x2488` | Peter Lawrence CMSIS-DAP Dapper Miser |
| `0x2500` | Satprod FastKey |
| `0x2505` | Bark It Up! ZettaLights |
| `0x2525` | First.AC MFSerial |
| `0x2552` | ProjectIota Electrolink |
| `0x256F` | 256 Foundation Ember One |
| `0x25F0` | Blue Wind 25 JxMake USB CDC-ACM Device |
| `0x2600` | Majenko Technologies chipKIT Lenny |
| `0x2609` | s-ol 0x33.board |
| `0x262A` | FreeJoy Team FreeJoy |
| `0x2635` | Sevinz GameBot |
| `0x2640` | The Odd Bit ppmjoy joystick driver |
| `0x2700` | mo10 Pigeon |
| `0x2702` | Inverse Path USB armory |
| `0x2730` | Misaka0x2730 RubyLink |
| `0x2777` | uCANDevices uCCB (CAN USB CONVERTER) |
| `0x2778` | uCANDevices CANFD (FD-CAN USB Converter) |
| `0x2779` | uCANDevices LUC (LIN USB CONVERTER) |
| `0x2800` | Entropic Engineering Triangulation |
| `0x2801` | Entropic Engineering Object Manipulation |
| `0x2812` | RGB.NET WS2812B-controller |
| `0x2828` | HedgeTech TotTag |
| `0x2830` | TScatliff MSD Interposer |
| `0x2832` | librtlsdr RTL2832U based SDR |
| `0x2882` | sanjay900 Ardwiino |
| `0x2883` | sanjay900 Ardwiino Bootloader |
| `0x2900` | Michael Kuc U2FDevice |
| `0x2925` | Dan Nixon Aya |
| `0x2982` | toxygen.net ppk |
| `0x2983` | toxygen.net printer |
| `0x29AF` | ElectronicsFlip NES to USB Adapter |
| `0x29B0` | ElectronicsFlip Genesis to USB Adapter |
| `0x2A00` | mooware Wii USB adapter |
| `0x2A01` | mooware SNES USB adapter |
| `0x2AEE` | ymkn neige |
| `0x2AEF` | ymkn Henge40 |
| `0x2AF0` | ymkn Slab40 |
| `0x2AF1` | ymkn YuTofu |
| `0x2AF3` | ymkn GamingSquare |
| `0x2AF4` | ymkn PinKey |
| `0x2AF5` | ymkn KeeBar |
| `0x2AF6` | ymkn YM60JIS |
| `0x2AF7` | ymkn YM60A |
| `0x2B00` | gamaPhy Freedom-3k |
| `0x2BAD` | Cardona Bits Haxophone |
| `0x2C93` | Cherry Pi Team 11411 Optical Flow Board |
| `0x2E7F` | Windfisch Midikraken Bootloader |
| `0x2E80` | Windfisch Midikraken |
| `0x2F07` | OrangeFox86 DreamcastControllerUsbPico |
| `0x2FA2` | muru.global 2FAK Security Key |
| `0x3000` | lloyd3000 USB device |
| `0x3003` | Captain Credible Midimalism |
| `0x3020` | Dr. Gusman LogicAnalyzer |
| `0x3030` | FLINT USB-I2C Bridge Board |
| `0x3070` | MegaCMD MegaCommand |
| `0x3100` | OpenSimHardware Pedals & Buttons Controller |
| `0x3141` | CrumpSpace CrumpSpace CrumpS2 - CircuitPython |
| `0x3142` | CrumpSpace CrumpSpace CrumpS2 - UF2 Bootloader |
| `0x316D` | Asahi Linux m1n1 CDC-ACM Proxy/Console Device |
| `0x317E` | Codecrete Wirekite |
| `0x3210` | OSH Lab, LLC Magic Keys |
| `0x3249` | ammlab.org USB MIDI to Sync Converter |
| `0x3252` | Paul Targett S2 Module Clip Wroom |
| `0x3253` | Paul Targett S2 Module Clip Wrover |
| `0x3270` | Tron Guy Labs M122/3270 USB keyboard |
| `0x32B1` | Shik N32B |
| `0x3304` | 3-Key-Ecosystem 2key-keyboards |
| `0x3305` | 3-Key-Ecosystem 4key-keyboards |
| `0x3306` | 3-Key-Ecosystem 6key-keyboards |
| `0x3320` | Meet je stad! MJS2020: Meetjestad sensor station 2020 |
| `0x3321` | Baker-link-Lab Baker link. Dev |
| `0x3333` | LabConnect Digitalnetzteil |
| `0x3336` | Grigory Avdyushin Franky 36 keyboard |
| `0x3375` | kien242 Peach v1 keyboard |
| `0x3434` | Konamiman Rookie Drive in device mode |
| `0x3442` | Orbcode Orbtrace-boot |
| `0x3443` | Orbcode Orbtrace |
| `0x345B` | kinX USB Hub |
| `0x345C` | kinX Keyboard Controller |
| `0x3500` | TinyCircuits Thumby |
| `0x3501` | TinyCircuits Thumby Color |
| `0x3613` | Sutajio Ko-Usagi (Kosagi) Precursor |
| `0x3690` | Kigakudoh TouchMIDI32 |
| `0x3701` | 13-37.org electronics Infinite Noise TRNG |
| `0x3706` | eta DMG1083 panel driver |
| `0x3739` | Pico 3x3 |
| `0x3757` | ladniy Jiran |
| `0x3845` | Eeble MyFirstMacroPad |
| `0x3919` | Sinara Thermostat |
| `0x391A` | Sinara Thermostat-EEM |
| `0x392F` | Sinara Stabilizer |
| `0x3933` | Sinara Booster |
| `0x3939` | Diva Engineering BOKAKA |
| `0x3A00` | Ploopy Mouse |
| `0x3DBD` | Fabio Pugliese Ornellas 3D Printed Keyboard 2 |
| `0x3F00` | Bushel Basket Musical Instruments MIDI Anglo Concertina |
| `0x4000` | auverdion freeDSP-aurora |
| `0x4004` | Paul Hampson Smart Coaster |
| `0x4040` | Dan Rodrigues (@drr) PicoHX iCE40 SPI programmer |
| `0x4096` | CynaraKrewe Cynara |
| `0x4121` | LibreVNA LibreVNA |
| `0x4122` | LibreVNA LibreCAL |
| `0x4126` | ManoShu BareDev rev1 |
| `0x414C` | Adi Linden uino-32u4 |
| `0x414D` | Adi Linden *uino-1284p |
| `0x4153` | sakharov.me USBLPT |
| `0x4200` | Badge.Team MCH2022 badge |
| `0x4201` | 42. Keebs Sub-Etha Pad |
| `0x4202` | 42. Keebs Mysteripad |
| `0x4203` | 42. Keebs Frood |
| `0x4223` | fluepke fluepdot |
| `0x4237` | XenGi noisy cricket |
| `0x4242` | Komakallio Astrophotography Community KomaHub Remote Power Switch |
| `0x4252` | Bruin Spacecraft Group RAPID-0 |
| `0x4256` | CuVoodoo BusVoodoo multi-protocol debugging adapter |
| `0x4269` | cnlohr MAGFest 2024 Swadge |
| `0x42A0` | SkeletonKBD Nehan42 |
| `0x42A1` | Michael Loose DualStepperController |
| `0x4321` | mooltipass Mooltipass Mini BLE |
| `0x4322` | mooltipass Arduino Sketch |
| `0x4353` | Idle Hands Dev. HCC Module (SAMD21G18A) |
| `0x4356` | CuVoodoo CuVoodoo firmware |
| `0x43AC` | BEKOS Muse |
| `0x4443` | j1rie IRMP_STM32 Bootloader |
| `0x4444` | j1rie IRMP_STM32 |
| `0x4445` | j1rie IRMP_STM32_KBD |
| `0x4446` | j1rie IRMP_PICO |
| `0x4513` | Adam Gausmann Riemann |
| `0x4520` | Karoly Molnar JTAG Switch |
| `0x4545` | SlothCo Enterprises USB-Teletype Adapter |
| `0x455A` | JellyTitan ErgoDonk Zero Split Keyboard |
| `0x4567` | 4project Plug-n-Play SAMD21 Development Board |
| `0x4573` | Adam Gausmann WindowMaster |
| `0x4646` | SmartPID SPC1000 |
| `0x4649` | e3w2q SU120 |
| `0x4650` | e3w2q RE5 |
| `0x4651` | e3w2q te96 |
| `0x4652` | e3w2q Ukiha |
| `0x4653` | e3w2q MX2U! |
| `0x4654` | e3w2q Kasumigasane |
| `0x4655` | Keytectonix Pangaea |
| `0x4665` | Nick Spiker ferros |
| `0x4704` | xwhatsit capacitive keyboard controller xwhatsit capacitive keyboard |
| `0x4747` | Johann Hanne Johann‘s Receiver Board |
| `0x4748` | Kate Gray GHETT-iO Bootloader |
| `0x4750` | Chris Pavlina (c4757p) C4-x computer (development interface) |
| `0x4757` | Chris Pavlina (c4757p) WCP52 Gain/Phase Analyzer |
| `0x4801` | Wojciech Krutnik NVMemProg |
| `0x4803` | XenGi mkbd65 |
| `0x484C` | cqa02303 HIDloom Keyboard |
| `0x4850` | Hotp4ck Hotp4ck60 |
| `0x4853` | Probe-rs HS Probe |
| `0x4884` | Gabriel Guerrer RAVA RNG |
| `0x4919` | Nick Brassel (@tzarc) Djinn Mechanical Keyboard |
| `0x4920` | Nick Brassel (@tzarc) Ghoul Mechanical Keyboard |
| `0x4921` | Nick Brassel (@tzarc) Kobold Macropad |
| `0x4A01` | EOA-electronics MidiPression |
| `0x4A46` | HYPERGLITCH Ltd JellyfishOPP |
| `0x4A4F` | xsyetopz OpenJoystickDriver Generic HID Gamepad |
| `0x4A62` | James Cahill Genoswitch Measurement Platform (City of London UK) |
| `0x4B42` | Gadget Project HID bridge |
| `0x4B4B` | smw42 Power Meter |
| `0x4B56` | Christiaan M. Geldenhuys Kleinvoet |
| `0x4C48` | uni-due.ies ElasticNode |
| `0x4C53` | Lambert Sartory Clavier |
| `0x4C60` | MightyPork GEX USB module |
| `0x4C61` | MightyPork GEX wireless dongle |
| `0x4D01` | heathen.pro MIDAL MIDI Interface |
| `0x4D41` | Robotics Masters MM1 Hat - MSC Bootloader |
| `0x4D42` | Robotics Masters MM1 Hat - SeeSaw |
| `0x4D43` | Robotics Masters MM1 Hat - CircuitPython |
| `0x4D44` | Robotics Masters MM1 Hat - Bootloader |
| `0x4D45` | Robotics Masters MM1 Hat - Arduino |
| `0x4D49` | Mitchell Software Solutions LLC MIDIMaster Virtual Microphone |
| `0x4D4D` | Matosic Macropad |
| `0x4D53` | mindsensors.com NXTCam5 |
| `0x4D58` | MixlarLabs Mixlar Mix |
| `0x4D69` | Keir Fraser Greaseweazle |
| `0x4DDD` | Oak Development Technologies CP Sapling Development Board |
| `0x4DDE` | Oak Development Technologies CP Sapling Development Board w/ SPI Flash |
| `0x4DDF` | Oak Development Technologies CP Sapling Development Board Revision B |
| `0x4DF0` | Oak Development Technologies PixelWing ESP32-S2 |
| `0x4DF1` | Oak Development Technologies BREAD 2040 |
| `0x4DF2` | Oak Development Technologies Cast-Away RP2040 |
| `0x4DF6` | Oak Development Technologies RPGA Feather |
| `0x4E00` | Vincent Manoukian (Acapeo) Acapeo LedExtender |
| `0x4E42` | noknok noknok USB module bootloader |
| `0x4E4E` | noknok noknok USB module |
| `0x4E56` | Neelverse Studios Private Limited Neelverse UVC Camera |
| `0x4EB1` | hebitawa re-square |
| `0x4F45` | Open Embedded Probe OEP probe (OpenEmbeddedProbe firmware) |
| `0x4F4A` | OneKeyLtd OneKey Bootloader |
| `0x4F4B` | OneKeyLtd OneKey Device |
| `0x4F4C` | OneKeyLtd OneKey Multi-protocol Interface |
| `0x4F54` | OpenTX Radio Transmitter |
| `0x4FB3` | scd31 RCade Display Adapter |
| `0x5000` | Lab11 TotTag |
| `0x5020` | joleuger vuinputd |
| `0x5038` | frotz.net mdebug rswd protocol |
| `0x5039` | frotz.net lpcboot protocol |
| `0x5050` | trebb ISO50 |
| `0x5054` | ES ES Timer |
| `0x5070` | SoloKeys SoloHacker |
| `0x50B0` | SoloKeys SoloBoot |
| `0x50DA` | fizzyade SFP Doctor |
| `0x5114` | Silica Silver |
| `0x5150` | 5150Lab PowerMon |
| `0x5153` | SparkType SparkType Keystroke Injector |
| `0x516A` | digitarhythm TReK GT40Plus PARALLAX |
| `0x516B` | digitarhythm TReK 40Plus SYMMETRIA |
| `0x516C` | digitarhythm TReK G70 CLAVIS |
| `0x5222` | telavivmakers attami |
| `0x5242` | Zynthian zynthian |
| `0x5252` | PskOnline PSK-X20 PPG Waveform Sensor |
| `0x5284` | joric nRFMicro Bootloader |
| `0x5304` | Klingler USB thermocouple logger |
| `0x5305` | Klingler Raspberry Pico USB GPIB adapter |
| `0x5350` | X41 SpacePort |
| `0x5385` | brianthe.dev Pulse85 HW |
| `0x5389` | Infinite USB Test Generic USB HID Controller |
| `0x53A0` | 0x53A (Lukas Rieger) Diskomator 9000 Pro Max |
| `0x53C0` | SatoshiLabs TREZOR Bootloader |
| `0x53C1` | SatoshiLabs TREZOR |
| `0x5432` | Open Programmer Open Programmer |
| `0x5454` | Tiny Tapeout Tiny Tapeout Breakout Board |
| `0x5457` | Openlab.Taipei Taiwanduino |
| `0x5501` | scumnoodlesociety asceension |
| `0x5502` | scumnoodlesociety dme4x5 |
| `0x5503` | scumnoodlesociety dme5x6 |
| `0x5504` | scumnoodlesociety dme6x6 |
| `0x5505` | SKKK Ino |
| `0x554D` | Max Maisel USB-Mux |
| `0x554E` | Max Maisel Split-Ergo Keyboard |
| `0x5555` | Frozen Electronics SpeedHonator |
| `0x557D` | FourierIndustries SSTuino II |
| `0x5687` | ECCN M0 |
| `0x569C` | OpenAudioGear USB3 Super Speed microcontroller board |
| `0x570F` | cnlohr CH570 funprog |
| `0x5716` | ResNullius Lednice |
| `0x571C` | StreetoArcade PancadariaStick |
| `0x573C` | Dima LPP - Linear Path Platform |
| `0x5740` | ArduPilot.org autopilot |
| `0x5741` | ArduPilot.org autopilot |
| `0x5750` | Woyta Woyta-Pad |
| `0x5800` | yuchi Lily58 Pro Rev2 |
| `0x5801` | yuchi Lily58 Lite Rev3 |
| `0x5858` | LNE LNE Keyboard Device |
| `0x5955` | sckzor Sckzor&apos;s 2.4ish GHz radar |
| `0x595A` | MatheusFraguas RetroZord |
| `0x5A0C` | Marcus Comstedt (zeldin) Orange Cartridge Bootloader |
| `0x5A22` | ikari_01 sd2snes |
| `0x5A52` | ZFR KBD RP2.65-F Keyboard |
| `0x5A5A` | EUROVIBES sertest-ng |
| `0x5AB1` | geek-rabb1t Futaba |
| `0x5AB2` | geek-rabb1t GR-Trackpad65 experiment kit |
| `0x5AF0` | Good Stuff Department OrangeCrab Bootloader |
| `0x5AF1` | Good Stuff Department ButterStick DFU Bootloader |
| `0x5AFE` | skyBlip skyBlip |
| `0x5BF0` | Foosn Fomu |
| `0x5C00` | Guri-Tech c-pro-micro |
| `0x5C51` | BlueSCSI BlueSCSI V2 |
| `0x5E7C` | sev.dev Chrumm keyboard |
| `0x5F00` | Andreas Sandberg RF Chameleon |
| `0x6000` | Pulsar Heavy Industries Cenx4 |
| `0x600D` | Makdaam N93 Interface |
| `0x6036` | kelvinA WK-50 Trackball Keyboard |
| `0x6050` | FPGA-Computer STM8 Gyrostick |
| `0x6060` | Ron Pedde r68 65% keyboard |
| `0x6102` | Bitaxe ASIC Bitcoin miner |
| `0x6110` | Six Pixels SEEQ MIDI and CV/Gate sequencer |
| `0x6262` | xwhatsit capacitive keyboard controller xwhatsit capacitive keyboard QMK VIA |
| `0x6362` | Sadale.net (jan Sate) ilo nena |
| `0x6369` | swaziloo cinque |
| `0x6370` | Eric Brombaugh SPI_Fiend |
| `0x63C1` | ljsebald GekiPi |
| `0x6400` | MostlyWhat Systems AstroX64 Steering Wheel |
| `0x6404` | rgrr/HardySoft YAPicoprobe CMSIS-DAP v2 |
| `0x6464` | Electric Exploits Shinewave |
| `0x6502` | jj1bdx avrhwrng v2rev1 |
| `0x6570` | Iowa Scaled Engineering, LLC CKT-AVRPROGRAMMER |
| `0x6660` | Maebe.Me The Button |
| `0x6666` | Talpa Chen VSFLogic |
| `0x6667` | SensePost Universal Serial aBUSe - Generic HID |
| `0x6687` | FMDX.org TEF668X Headless USB Tuner |
| `0x6742` | NPK Cubitel Atomic Force Microscope |
| `0x6789` | Pounamu Electronics RE_SWC |
| `0x6800` | pdaXrom N64cart |
| `0x6804` | saybur Hootswitch |
| `0x6809` | Tach Radio Doppelganger |
| `0x6870` | yukke.org hidpin |
| `0x68A0` | SkeletonKBD Frtost68 |
| `0x6939` | bonzi bongopad |
| `0x6948` | MySensors Sensebender Gateway BootLoader |
| `0x6949` | MySensors Sensebender Gateway |
| `0x6969` | Ibnu D. Aji (@ibnuda) Squiggle Mechanical Keyboard |
| `0x6970` | Skye Green Tiramisu |
| `0x6971` | Skye Green Mascarpone |
| `0x69A1` | Marius Kavoliunas Macro25 (mk-macro-01) |
| `0x6ABB` | gabbro-foss Gabbro virtual FIDO2 authenticator |
| `0x6BCF` | blaste Gameboy Cart Flasher |
| `0x6D00` | MNT Research MNT Reform Keyboard 2.0 |
| `0x6D01` | MNT Research MNT Reform Keyboard 3.0 |
| `0x6D02` | MNT Research MNT Reform Keyboard 4.0 |
| `0x6D03` | MNT Research MNT Reform Trackball 1.0 |
| `0x6D04` | MNT Research MNT Reform Trackball 2.0 |
| `0x6D05` | MNT Research MNT Reform Trackpad 1.0 |
| `0x6D06` | MNT Research MNT Pocket Reform Input 1.0 |
| `0x6D07` | MNT Research MNT Pocket Reform System Controller 1.0 |
| `0x6D08` | MNT Research MNT Reform Next System Controller 1.0 |
| `0x6D09` | MNT Research MNT Reform Camera |
| `0x6E00` | Cowlark Technologies FluxEngine |
| `0x6E01` | Kodachi 6 14 MacroPaw KnGXT |
| `0x6E02` | Kodachi 6 14 MacroPaw KnGYT |
| `0x6E6E` | Allorx Orions Hands |
| `0x6F00` | electric.kitchen Pulsar Buddy |
| `0x6F78` | Oxide Keyboard oxikey HID |
| `0x6F79` | Oxide Keyboard oxikey MSC |
| `0x7000` | Secalot Dongle |
| `0x7001` | Secalot Bootloader |
| `0x7042` | Bruce Biotech dbu |
| `0x7050` | Positron Electronic OSUpad |
| `0x7070` | Drake Consulting s.r.o. USB to I2C bridge |
| `0x70B1` | Sutajio Ko-Usagi (Kosagi) Tomu |
| `0x7102` | Benjamin Shockley Mini SAM M0 |
| `0x7103` | Benjamin Shockley Fig Pi |
| `0x7110` | DG1PSI PicoDitDah |
| `0x7111` | David Cooper Tomato Radio Automation Button Box |
| `0x7150` | ElectronicCats Hunter Cat NFC |
| `0x71C4` | Lectrobox LectroTIC-4 4-Channel Pulse Timestamper |
| `0x71C5` | Lectrobox PG-4 4-Channel Pulse Generator |
| `0x7200` | c0bra-dev Gemini |
| `0x7230` | tomoya723 FULLMONI-WIDE |
| `0x7272` | TonyWu FLUXPAD Analog Keypad |
| `0x7274` | Emil Fresk Rusty Tracker |
| `0x7331` | Dangerous Prototypes Bus Pirate Next Gen CDC |
| `0x7332` | Dangerous Prototypes Bus Pirate Next Gen Logic Analyzer |
| `0x7366` | mucki.dev SigFeather |
| `0x7370` | snopf snopf USB Password Token |
| `0x7371` | snopf snopf-bootloader |
| `0x7380` | Invector Labs AB Challenger 840 BLE - Bootloader |
| `0x7381` | Invector Labs AB Challenger 840 BLE - CDC Serial port |
| `0x7382` | Invector Labs AB Challenger RP2040 SUBGHz - CDC Serial port |
| `0x7388` | skuep All-In-One-Cable |
| `0x73EA` | Etherkit Empyrean Alpha |
| `0x73EB` | Etherkit Empyrean Beta |
| `0x7401` | Beststream-jp Tool_CDC |
| `0x7402` | Movuino OpenHealthBand |
| `0x7410` | wcysite TF Mux |
| `0x7522` | Makbit MIDI2USB |
| `0x7530` | PotentialLabs Refflion - IoT Development Board - Bootloader |
| `0x7531` | PotentialLabs Refflion - IoT Development Board - Sketch |
| `0x7551` | The Tessel Project Tessel 2 |
| `0x7600` | Kameleon Kameleon Core |
| `0x7665` | denhac Snack vending machine controller board |
| `0x7672` | Tecsmith VR61 Keyboard |
| `0x7689` | LGDXRobot LGDXRobot2 |
| `0x7690` | SlimeVR SlimeNRF Receiver |
| `0x7691` | SlimeVR SlimeNRF Receiver Bootloader |
| `0x7692` | SlimeVR SlimeNRF Tracker |
| `0x7693` | SlimeVR SlimeNRF Tracker Bootloader |
| `0x7700` | marwedels.de IRUSB |
| `0x7701` | marwedels.de Audiomux |
| `0x7702` | marwedels.de UniversalboxARM DFU bootloader |
| `0x7777` | circuitvalley USB IO Board V3 |
| `0x7778` | circuitvalley USB IO Board V3 Bootloader |
| `0x7787` | circuitvalley GPSDO USB UART Converter |
| `0x7800` | Vespid Orbweaver |
| `0x7877` | circuitvalley AD4351 USB RF Signal Generator |
| `0x7888` | circuitvalley CHC5 USB Camera |
| `0x7A01` | zhiayang Mikoto nRF52840 |
| `0x7A3B` | Jacob Waters Macro Pad |
| `0x7AA0` | UniversalScientificTechnologies AIRDOS04 - Advanced Radiation Monitoring for Aircrafts |
| `0x7BD0` | pokey9000 Tiny Bit Dingus |
| `0x7C88` | Redrield CATS mrrp |
| `0x7D00` | RIOT Operating System RIOT standard peripherals |
| `0x7D01` | RIOT Operating System RIOT Test PID |
| `0x7D02` | RIOT Operating System riotboot DFU Bootloader |
| `0x7E7A` | Blazing Design SACADA Mini |
| `0x7EAD` | Martin Pietryka TheadMouse |
| `0x8000` | Autonomii NODii 2 |
| `0x8011` | OpenPrinting Printer Application Framework (PAPPL) |
| `0x8048` | Codecrete USB-to-serial adapter |
| `0x8050` | Maurice Rowe Sim Pedals |
| `0x8051` | TomKeddie SAMD21G18 PMOD module |
| `0x805A` | ElectronicCats Bast BLE |
| `0x8080` | anarion80 Porcupine Keyboard |
| `0x8086` | MisfitTech Nano Zero Bootloader |
| `0x8087` | MisfitTech Nano Zero |
| `0x80A0` | Mate Szalay-Beko (SyMat) ATMega-Z80 - an 8 bit hobby computer |
| `0x8100` | bjonnh DINOctopus |
| `0x8101` | bjonnh Theremin (provisional title) |
| `0x8118` | Roberto Alsina kv |
| `0x8123` | Danyboard Danyboard M0 bootloader |
| `0x812A` | Danyboard Danyboard M0 |
| `0x813A` | MickMad HACK Bootloader |
| `0x813B` | MickMad HACK Sketch |
| `0x8191` | Metal Boards Insane MacroPad |
| `0x8192` | Prototyping Corner Ultra-WideBand Feather |
| `0x8242` | Tom Wimmenhove Electronics NBS-DAC 192/24 UAC1 |
| `0x8243` | Tom Wimmenhove Electronics NBS-DAC 192/24 UAC2 |
| `0x8250` | usbip-win Virtual HUB |
| `0x8251` | Frans van Dorsselaer USB/IP Stub Device |
| `0x8300` | doankt Poomsae Controller |
| `0x8472` | Shantea Controls OpenDeck |
| `0x8661` | ProgHQ ProgHQ TL866 programmer |
| `0x8662` | ProgHQ ProgHQ TL866II Plus programmer |
| `0x8738` | JeffPeng UniversalIO |
| `0x8777` | circuitvalley KLIP30 USB Fast Pulse Generator |
| `0x8787` | MVMC-lab MVMC-lab_M128_V3 |
| `0x8788` | MVMC-lab MVMC-lab_M3_V1 |
| `0x8800` | Zolex Labs Ltd Zolex Superboard |
| `0x8801` | hanyazou 16F1454 PIC programmer |
| `0x8816` | Koen De Vleeschauwer ARM CAN tool |
| `0x8844` | munia.io MUNIA |
| `0x8851` | Till&apos;s USB Gadgets The Tic-Nic PTP Adapter with GPS and Hardware I/O |
| `0x8885` | TKEY-USB-V2 |
| `0x8886` | TP-1 |
| `0x8887` | MTA-USB-V1 |
| `0x8888` | Blinkinlabs POV Pendant |
| `0x8889` | Blinkinlabs POV Pendant (bootloader) |
| `0x88BF` | Topten Software MacroPad RAW HID Firmware |
| `0x88EA` | Etherkit Empyrean Alpha (bootloader) |
| `0x88EB` | Etherkit Empyrean Beta (bootloader) |
| `0x8989` | CustomSimEchoSystem Universal Interface CustomSimEchoSystem Universal Interface |
| `0x8990` | Jancgk KVM-Card-Mini K/M port |
| `0x8991` | Jancgk KVM-Card-Mini Data port |
| `0x8B00` | ReSwitched Libtransistor Serial Console |
| `0x8B17` | Andrei Lukovenko Dendy-compatible Joystick USB HID Interface |
| `0x8CAE` | takayoshiotake Octave 12-key macropad |
| `0x9000` | ajs256 Hack Club Sprig CircuitPython |
| `0x9001` | CombineReality North Star Integrator |
| `0x9021` | Connected Community Hackerspace ESPlant |
| `0x9090` | TinyGo USB |
| `0x9101` | Dave Madison Sim Racing Shifter |
| `0x9102` | Dave Madison Sim Racing Pedals |
| `0x9120` | GreenKeys Gravity36 |
| `0x9121` | GreenKeys Gravity45 |
| `0x9200` | Josh Robson Chase Polymer Keyboard |
| `0x9209` | djvs aek |
| `0x9210` | kunichiko Mimic X |
| `0x9211` | kunichiko Mimic X Bridge |
| `0x9229` | Smoria Smoria&apos;s Knob |
| `0x9317` | Sutajio Ko-Usagi (Kosagi) Palawan-Tx |
| `0x9696` | ARTSEY The Paintbrush |
| `0x9701` | Hideez Group Inc. Hideez Wallet |
| `0x9779` | Raul Rodrigo Unicorne |
| `0x9801` | hanyazou BBQ10 USB keyboard |
| `0x9998` | OPOLO OPOLO Cosmos Bootloader |
| `0x9999` | OPOLO OPOLO Cosmos Firmware |
| `0x9DB5` | PD Buddy Sink |
| `0xA000` | LeeNX Consultancy Gamepad projects |
| `0xA001` | PostalPortal LLC d.b.a PostalPoint® PostalPoint Kiosk Controller |
| `0xA002` | PostalPortal LLC d.b.a PostalPoint® PostalPoint Parcel Dimensioner |
| `0xA003` | PostalPortal LLC d.b.a PostalPoint® PostalPoint Barcode Scanner |
| `0xA007` | dorucazan IR Multimedia Keyboard |
| `0xA008` | SkeletonKBD Skeleton8 |
| `0xA033` | area0x33 Memtype |
| `0xA0D1` | Aodrulez blueTag |
| `0xA100` | KB LES Narsil analog breakout |
| `0xA107` | /// pini.grigio /// volume.control |
| `0xA10C` | KB LES Aminoacid Synthesizer |
| `0xA119` | Ning Wang simpleFE |
| `0xA165` | alglabnet Let&apos;s Swamp |
| `0xA170` | PLA LABS ALT Avatar Motion Controller |
| `0xA182` | Solder Party RP2040 Stamp |
| `0xA1E5` | Atreus Keyboards Atreus Keyboard |
| `0xA1F1` | Kitsune Keyboards Shapeshifter 4060 |
| `0xA200` | 0x0h YaDRV - yet another stepper motor driver |
| `0xA2B2` | LibreBridge Mini |
| `0xA320` | Raffles LrE-6 |
| `0xA380` | Raffles LrTMAX |
| `0xA3A4` | MK::Box MK::Kbd |
| `0xA3A5` | MK::Box MK::Kbd Bootloader |
| `0xA420` | Team Walrus Tusk |
| `0xA455` | jlwang Foundation Vault 35 Katana |
| `0xA516` | digitarhythm TReK GT40 DONUMCOELI |
| `0xA55A` | Forever Young Software USB: The Universal Serial Bus: ATTINY2313 |
| `0xA5B3` | badjeff Chloroplast Bootloader |
| `0xA602` | Robotips RTBoard |
| `0xA69A` | Uncommon Group Lagrange USB-UART Bridge |
| `0xA6E1` | Krypt0ph0ne Agent Micro |
| `0xA711` | Hans Hübner ATLAS-11 |
| `0xA770` | Pizza Robotics uNav2 - Robot motor controller |
| `0xA7EA` | area3001 Knixx SW04 |
| `0xA800` | sowbug.com WebLight |
| `0xA805` | Pragmatic Keyboard 務實鍵盤 Pragmatic Keyboard 務實鍵盤 |
| `0xA831` | Diva Engineering Diva Card |
| `0xAA00` | Serg Oskin LinuxCNC HID Extender |
| `0xAA0B` | Open Bionics Open Bionics |
| `0xAA61` | apfelaudio apfelbug |
| `0xAA62` | apfelaudio tiliqua |
| `0xAA88` | Kipp Cannon ALLPRO88 USB |
| `0xAAAA` | Prokey Optimum Hardware Wallet |
| `0xAB0C` | bad_alloc Heavy Industries SPIFlashProgrammer |
| `0xAB3D` | 3DArtists Alligator board |
| `0xABAD` | SmartGauges Usb2Most(USB to MOST adapter) |
| `0xABB8` | CoinWISE SafeWISE Bootloader |
| `0xABB9` | CoinWISE SafeWISE |
| `0xABBA` | CoinWISE SafeWISE (HID) |
| `0xABC0` | Omzlo The Omzlo USB controller |
| `0xABC1` | ABCKEY.COM ABCKEY |
| `0xABCD` | Sandeepan Sengupta Domino |
| `0xABD1` | OpenMV Cam |
| `0xABED` | Lixie Labs Sensory Bridge |
| `0xABEE` | Good Great Grand Wonderful Levels54 |
| `0xABFF` | MelanieT PiLomar stepper driver |
| `0xAC1D` | jgn35 Soft KVM USB device |
| `0xACAB` | Architeuthis Flux Jumperless |
| `0xACC1` | Accelerando Redsoil |
| `0xACD0` | Warranty Voids WarrantyVoids MIDI 2 CV |
| `0xACDC` | Gediminas Zukaitis midi-grid |
| `0xACE0` | MediaCode, LLC Custom HID Controller |
| `0xACE1` | MediaCode, LLC Custom HID Controller |
| `0xACE5` | SimAces Panel Ace |
| `0xACEB` | Open Lighting Project rp2040-dmxsun |
| `0xACED` | Open Lighting Project Ja Rule Device |
| `0xACEE` | Open Lighting Project Ja Rule Bootloader |
| `0xAD0E` | AdvaScope s.r.o. USB-C Readout |
| `0xAD10` | Gurnarok Teensy Motor Controller |
| `0xADA1` | OpenAudioGear ADATface - ADAT USB2 high speed audio interface |
| `0xADA2` | OpenAudioGear ADATface - ADAT USB2 high speed audio interface |
| `0xADB0` | tibounise ADB/USB converter |
| `0xADDA` | MicroPython MicroPython Boards |
| `0xADF0` | Axeia SuperMini NRF52840 |
| `0xAE01` | AstroMeters AMFOC01 |
| `0xAE6E` | Radagast.org OpenTNC |
| `0xAE86` | Modos Tech Glider EPD Controller |
| `0xAF01` | blus-audio USB-I2S bridge |
| `0xB000` | SoloKeys Solo 2 Bootloader |
| `0xB001` | pkoscik Zephyr based GRUB boot selector switch |
| `0xB003` | cnlohr CH32V003 RISC-V USB Firmware Bootloader |
| `0xB007` | Konsgn Global_Boot |
| `0xB008` | NU Synths MIDICTRL |
| `0xB009` | NU Synths MIDICTRL Bootloader |
| `0xB00B` | CrapLab Random USB Device |
| `0xB010` | Second Bedroom Game Bub |
| `0xB011` | starlarkus Game Boy Link Adapter |
| `0xB01D` | WyoLum VeloKey |
| `0xB058` | Model B, LLC Holoseat |
| `0xB0B0` | Monero Hardware Bootloader |
| `0xB0DE` | OHMMUSICCOLLECTIVE OHMMIDIMOD |
| `0xB100` | ptrandem iBizi |
| `0xB126` | noForth noForth t CDC driver |
| `0xB182` | Solder Party BBQ20KBD |
| `0xB195` | flehrad Big Switch PCB |
| `0xB196` | Kauhsa Analog Dance Pad |
| `0xB1C0` | tinyVision.ai Inc. pico-ice |
| `0xB1C1` | tinyVision.ai Inc. pico2-ice |
| `0xB210` | BlueKeyes Pico 12VRGB HID Controller |
| `0xB2A8` | cepetr Blue2Joy |
| `0xB420` | vitaly.codes WebUSB NeoPixel Controller |
| `0xB747` | Raffles LrT16 |
| `0xB803` | UIAP UIAPduino Pro Micro CH32V003 |
| `0xB806` | UIAP UIAPduino Pro Micro CH32V006 |
| `0xBA00` | KoBuss LLC Squash |
| `0xBA01` | KoBuss LLC Grabert |
| `0xBA55` | Lenwë Sáralondë Musician MIDI Pico Translator |
| `0xBAB0` | ElectronicCats BastWiFi |
| `0xBAB1` | ElectronicCats Meow Meow |
| `0xBAB2` | ElectronicCats CatWAN USB-Stick |
| `0xBAB3` | ElectronicCats Bast Pro Mini M0 |
| `0xBAB4` | ElectronicCats CatWAN Citizen |
| `0xBAB5` | ElectronicCats Magspoof V3 |
| `0xBAB6` | ElectronicCats Escornabot Makech |
| `0xBAB7` | ElectronicCats BastWAN |
| `0xBAB8` | ElectronicCats NFC Copy Cat |
| `0xBAB9` | ElectronicCats CatWAN Farmer |
| `0xBABA` | Alexey &apos;Cluster&apos; Avdyukhin Famicom Cartridge Dumper/Writer |
| `0xBABB` | ElectronicCats CatSniffer |
| `0xBABE` | brunofreitas.com STM32 HID Bootloader |
| `0xBAD1` | Gregory POTEAU CommLinkUSB |
| `0xBAD2` | Gregory POTEAU XLinkUSB |
| `0xBADA` | bad_alloc Heavy Industries MXKeyboard |
| `0xBADB` | bad_alloc Heavy Industries dragonBoot |
| `0xBADE` | Semarme SemarmeHID |
| `0xBB00` | keyplus split keyboard firmware |
| `0xBB01` | keyplus xusb bootloader |
| `0xBB02` | keyplus nRF24 wireless keyboard dongle |
| `0xBB03` | keyplus nrf24lu1p-512 bootloader |
| `0xBB05` | keyplus kp_boot_32u4 bootloader |
| `0xBB07` | Martin Pietryka BloomBoot UF2/DFU bootloader |
| `0xBB23` | BornHack 2023 NFC Badge |
| `0xBBB2` | rykkers bb w_less tact usb2 |
| `0xBBBB` | rykkers bb w_less tact |
| `0xBBD0` | clackups Chahor Adaptive Keyboard v1 |
| `0xBBD1` | clackups ESP32-S3 BLE-HID serial bridge |
| `0xBBD2` | clackups Smart Inclusive Keyboard |
| `0xBBEE` | thingy.jp BreadBee Perdita |
| `0xBC01` | kearney.dev fat8 |
| `0xBCBC` | Tygo van den Hurk (@Tygo-van-den-Hurk) Snowflake v2 |
| `0xBCDE` | Sandeepan Sengupta Neutrino |
| `0xBD00` | librelab librelab::Big Dot |
| `0xBD06` | NirvanaKbds Nirvana USB Keyboard |
| `0xBD10` | Ron Parker BDIOHID |
| `0xBDB1` | Event Horizon Illuminate |
| `0xBE7A` | AXIOM Beta |
| `0xBEBA` | serasidis.gr STM32 HID Bootloader |
| `0xBEDA` | Comrade Freeman LoRaMobGate |
| `0xBEE5` | Mikkel Jeppesen Vitamins Included |
| `0xBEEB` | Tom Seddon BeebLink |
| `0xBEEE` | SoloKeys Solo 2 |
| `0xBEEF` | Modal MC-USB |
| `0xC000` | Hrvoje Cavrak DeskHop |
| `0xC001` | Cynteract Alpha |
| `0xC003` | cnlohr CH32V003 RISC-V USB Generic HID Dongle |
| `0xC00B` | tritoke Teensy Keyboard Bridge |
| `0xC00C` | The Coren SIMPANEL flight sim instruments |
| `0xC00D` | The Coren Modern Cadet keyboard |
| `0xC011` | Comrade Comrade P1 |
| `0xC035` | Krzysztof Gawrys CH32X035 USB Power Meter |
| `0xC051` | Betrusted Simmel |
| `0xC0C0` | Geppetto_Electronics Orthrus |
| `0xC0C1` | Michael Bemmerl cookie-mouse |
| `0xC0CA` | Jean THOMAS DirtyJTAG |
| `0xC0D1` | Ryan Petris Cordial |
| `0xC0D3` | Samy Kamkar USBdriveby |
| `0xC0DA` | Monero Hardware Firmware |
| `0xC0DE` | KMRH Labs SBL Brain |
| `0xC0E0` | Coemorph USB Audio |
| `0xC0F5` | unethi PERswitch |
| `0xC0FE` | OgmaVision Multidevice Emulator |
| `0xC10C` | BobKerns AltoidMIDI |
| `0xC12C` | janoskut PicoSlave |
| `0xC1AA` | Proyecto CIAA Computadora Industrial Abierta Argentina |
| `0xC1B1` | Chibitronics Love-to-Code |
| `0xC1C1` | Studio-Lab Pico Expander |
| `0xC311` | bg nerilex GB-USB-Link |
| `0xC550` | Think Create ch55xduino |
| `0xC55A` | Think Create ch55xduino for audio devices |
| `0xC55C` | Think Create ch55xduino for CDC/HID combo devices |
| `0xC55D` | Think Create ch55xduino for HID devices |
| `0xC55E` | Think Create ch55xduino for MSD devices |
| `0xC64B` | Martin Oswald (do2mad) Pico USB Keyboard |
| `0xC680` | dedChar picoframeST |
| `0xC6BE` | Noteolvides CubeTouch |
| `0xC8A0` | Nessie Circuits Riotee board |
| `0xC8A1` | Nessie Circuits Riotee probe |
| `0xC8A2` | Nessie Circuits Riotee gateway |
| `0xC8C8` | RE-TikaRa Custom Keypad Board |
| `0xC9C9` | Project Daylily Inkclip |
| `0xCA01` | CANnectivity USB to CAN adapter |
| `0xCA02` | CANnectivity USB to CAN adapter (DFU) |
| `0xCA1C` | KnightOS Generic Hub |
| `0xCA1D` | KnightOS MTP Device |
| `0xCA20` | Calliope Calliope mini |
| `0xCA70` | Aki-nyan Squishy |
| `0xCA71` | Aki-nyan Squishy Bootloader |
| `0xCAC1` | Concept & Code Push-n-Turn 61 |
| `0xCAC2` | Concept & Code Push-n-Turn 31 |
| `0xCAC3` | Concept & Code Fad 4 |
| `0xCACE` | TheHolyHorse PowerSupply |
| `0xCADE` | Retrobeef Arcadinator |
| `0xCAEA` | Open Music Kontrollers Chimaera |
| `0xCAFE` | ii iigadget |
| `0xCB01` | keshikan-zume CureMIDI |
| `0xCB02` | keshikan-zume CureMIDI8 |
| `0xCB0B` | lophtware USB Type-C / PIC32 Breakout and Development Board |
| `0xCB65` | 0xCB Gemini |
| `0xCB74` | 0xCB Helios |
| `0xCC00` | Chinatsu Chiba (azulee/mokesura) CC Proto |
| `0xCC01` | Chinatsu Chiba (azulee/mokesura) CC Practice |
| `0xCC14` | trebb NaN-15 |
| `0xCC1D` | nickray Zissou |
| `0xCC3D` | Martin Pollow CCTrack IMU sensor |
| `0xCC86` | Manfred&apos;s Technologies Anastasia USB Bootloader |
| `0xCC87` | Manfred&apos;s Technologies SEGA Gamepad adapter |
| `0xCCAA` | UW Molecular Information Systems Lab PurpleDrop |
| `0xCCCC` | zeptoforth zeptoforth CDC Console |
| `0xCD07` | JackMacWindows PSG MIDI Device |
| `0xCDEF` | Sandeepan Sengupta Gravitino |
| `0xCE93` | rcls GPSFreak |
| `0xCEB0` | KG4LNE GE-FlashUSB |
| `0xCECE` | eAFAQ Omnimo nRF52840 |
| `0xCEDE` | Timothy Garcia espress0 |
| `0xCEDF` | Timothy Garcia breve |
| `0xCEE0` | Timothy Garcia thing |
| `0xCF20` | Smart Citizen SCK 2.0 |
| `0xD003` | cnlohr CH32V003 RISC-V USB Generic Raw Device |
| `0xD00D` | Monero Hardware Developer |
| `0xD017` | empiriKit empiriKit Controller |
| `0xD035` | cnlohr CH32X035 RISC-V USB &quot;003fun&quot; Example Applicaiton |
| `0xD067` | thingy.jp DogeDoge |
| `0xD08A` | doragasu MegaWiFi Programmer V |
| `0xD10D` | Diodes Delight Piunora |
| `0xD10F` | cmd.wtf Odiin Bootloader |
| `0xD110` | cmd.wtf Odiin |
| `0xD11D` | Koi Science DI-Lambda AVR |
| `0xD187` | PewPew uGame S3 |
| `0xD1A6` | Maximus64 OpenDIAG OBD2 Interface |
| `0xD1B5` | PewPew PewPew LCD |
| `0xD1B6` | PewPew uGame 22 |
| `0xD35C` | Derek Reynolds deskhopplus |
| `0xD369` | Vicharak Shrike-lite |
| `0xD3D8` | Duet3d Duet 0.8.5 |
| `0xD500` | Marc Riera Controller mod for Densha de GO! Plug & Play |
| `0xD666` | DaemonBite Game Controller Adapter |
| `0xD706` | SkyBean SkyDrop |
| `0xD9D0` | Martin Stumpf BusyBeacon |
| `0xDA15` | Elia Chiarucci DA15 |
| `0xDA42` | Devan Lai dap42 debug access probe |
| `0xDA7A` | Rodrigo Torres nanoBoot |
| `0xDAA0` | darknao btClubSportWheel |
| `0xDACA` | Daniele Caltabiano MidiPedalBoard |
| `0xDADA` | Rebel Technology OWL |
| `0xDB00` | Duccio Breschi (@duckyb) ETERNAL Keypad |
| `0xDB42` | Devan Lai dapboot USB DFU bootloader |
| `0xDB50` | Pease Technology, LLC DBS Controller |
| `0xDC21` | FPGA-Computer Dual Charger |
| `0xDCB0` | Robotics and Intelligent Mechatronics Laboratory (RobIn Lab), Faculty of Engineering, Kasetsart University, THAILAND Robin-Zeabus Acoustic Processing |
| `0xDCB1` | Robotics and Intelligent Mechatronics Laboratory (RobIn Lab), Faculty of Engineering, Kasetsart University, THAILAND Robin-Zeabus Weather Station |
| `0xDD83` | stm32dds DDS signal generator |
| `0xDDAA` | EllisGL DDAA Device |
| `0xDDDD` | Stephan Electronics OpenCVMeter |
| `0xDE1A` | Aaron Christiansen Delta Pico |
| `0xDE20` | Badbird5907 Deltaboard |
| `0xDE5C` | earth.li Desk Viking |
| `0xDEAD` | chaosfield.at AVR-Ruler |
| `0xDEAF` | CrapLab 4chord MIDI |
| `0xDEB0` | Anton Krug AK JTAG++ |
| `0xDEC1` | WeAct Studio RP2350B Core |
| `0xDECA` | OpenAudioGear DECA USB2 audio interface |
| `0xDED1` | ManCave Made Quark One |
| `0xDEDA` | Alexey &apos;Cluster&apos; Avdyukhin Famiclone and Sega Mega Drive controller to USB adapters |
| `0xDEED` | Aleh Zasypkin Kroneum |
| `0xDEF0` | Sinitax SXKBD |
| `0xDF00` | D.F.Mac. @TripArts Music mi:muz:tuch |
| `0xDF01` | D.F.Mac. @TripArts Music mi:muz:can |
| `0xDF02` | D.F.Mac. @TripArts Music mi:muz:can-lite |
| `0xDF03` | D.F.Mac. @TripArts Music mi:muz:ch55x |
| `0xDF11` | Nth Dimension LLC Signet |
| `0xDFA9` | marksard ms_m0ii040pcb |
| `0xDFAA` | marksard treadstone32 |
| `0xDFAB` | marksard numatray |
| `0xDFAC` | marksard alphamod32 |
| `0xE000` | libmcu MADI Device |
| `0xE001` | libmcu MADI Bootloader |
| `0xE004` | Bronco Space PROVES Kit RP2040 v4 |
| `0xE007` | VX4.NET / ck@vx4.net Exp.007 SECCID |
| `0xE07E` | AXIOM Remote |
| `0xE116` | Elijah Motornyy open-oscilloscope-stm32f3 |
| `0xE11A` | 3DP Accelerometer 3dpaxxel |
| `0xE128` | brxken128 passta |
| `0xE1E1` | Janne Virtala AutoPTT Sidekick |
| `0xE1EC` | FreeSRP FreeSRP |
| `0xE3CE` | EMCDev EMC |
| `0xE3E1` | StackRduino StackRduino M0 Pro Bootloader |
| `0xE3E2` | StackRduino StackRduino M0 Pro - Arduino |
| `0xE3E3` | StackRduino StackRduino M0 Pro - CircuitPython |
| `0xE4EE` | trebb keytee |
| `0xE500` | GitleMikkelsen Helios Laser DAC |
| `0xE501` | KERI&apos;s Lab KERIgoKBD v1 |
| `0xE502` | KERI&apos;s Lab KERIgoKBD v2 |
| `0xE582` | Electronya GT Sim Racing Wheel |
| `0xE667` | ERTOtech Refleks Lite DFU |
| `0xE668` | ERTOtech Refleks Lite XInput |
| `0xE669` | ERTOtech Refleks Lite DInput |
| `0xE739` | Envious media Env-KB |
| `0xEA69` | Gumulka Tinkering Projects guMIDI Controller |
| `0xEACC` | elmaczete_customs gtfog dfu |
| `0xEACD` | elmaczete_customs gtfog |
| `0xEAEA` | Pinscape Controller |
| `0xEAEB` | Pinscape Pico Controller |
| `0xEB01` | RobotMaker.club EB1 电脑一号 |
| `0xEBA7` | VictorGrigoryev USBscope |
| `0xEBEB` | iPXE Keyflogger |
| `0xEC00` | QuadState MCmacro |
| `0xEC02` | QuadState ECmacro |
| `0xEC1A` | Mateusz Nowak EclairM0 |
| `0xEDA1` | PiKVM PiKVM Composite Device |
| `0xEDA2` | PiKVM PiKVM HID |
| `0xEDA3` | PiKVM PiKVM HID Bridge |
| `0xEDED` | PinNudger Pin-Nudger |
| `0xEE00` | Explore Embedded SODA(SWD OpenSource Debug Adapter) |
| `0xEE02` | Explore Embedded Explore M3 VCOM |
| `0xEE03` | Explore Embedded Explore M3 DFU |
| `0xEE2C` | jaka USB2RS485 |
| `0xEEA1` | EngEmil.io AttentioLight-1 |
| `0xEEEE` | Villa Labs Lithe USB Arcade Stick Adapater |
| `0xEEF1` | openefi OpenEFI |
| `0xEF00` | 2231puppy E-Fidget |
| `0xEF0C` | Bohe Labs Giris |
| `0xEFEF` | Kevin Neubauer CircuitBrains Deluxe |
| `0xEFFA` | EffigyLabs atmega32u4-USB-LUFA-Bootloader |
| `0xEFFE` | EffigyLabs Control Pedal |
| `0xF000` | Uniti ARC |
| `0xF00D` | RomanStepanov Shifter/Pedals Adapter |
| `0xF00F` | denhac KiCad workshop |
| `0xF043` | MiniDexed MiniDexed |
| `0xF0C7` | OpenVVVF Control Module |
| `0xF0DA` | Hyades Command Deck |
| `0xF0F0` | hrmny.sh Serial WS2812 |
| `0xF103` | flow3r flow3r-firmware |
| `0xF10B` | flow3r flow3r-recovery |
| `0xF123` | Electrolama minik |
| `0xF12E` | Michael Bemmerl Feuermelder |
| `0xF150` | MavericSystems EFIS |
| `0xF16A` | uri_ba Cougar TQS adapter |
| `0xF16B` | Michi Hirczy F-16 ICP |
| `0xF16C` | uri_ba USB adapter for Vipercore&apos;s FCC3 Force Sensing Module |
| `0xF1D0` | mphi Pi Zero Security Key |
| `0xF1D2` | TheMaxMur RS-Key |
| `0xF1DE` | danielinux Fidelio U2F |
| `0xF1F0` | RAJlab RISCy Logger |
| `0xF209` | FriendTeamInc JukeBox V3 |
| `0xF314` | SharpCoder Teensycore |
| `0xF380` | Windsor Schmidt MD-380 Open Radio Firmware |
| `0xF3FC` | dRonin Flight controller-Lumenier Lux |
| `0xF480` | zbauman3 WorkerMouse |
| `0xF49A` | TimVideos.us & HDMI2USB.tv Projects FPGA Programmer & UART Bridge (PIC based Firmware) |
| `0xF4F4` | Eric Roy Gyroscreen |
| `0xF500` | Silicognition LLC M4-Shim |
| `0xF501` | Silicognition LLC M4-Shim Bootloader |
| `0xF502` | Silicognition LLC RP2040-Shim |
| `0xF540` | piers.rocks One ROM Bootloader |
| `0xF541` | piers.rocks pico1541 |
| `0xF542` | piers.rocks One ROM |
| `0xF600` | UTBDK Mr.Duppl |
| `0xF666` | Kevin Eshbach PEP |
| `0xF701` | KreativeKorp AnyKey |
| `0xF723` | KreativeKorp Two Thirds California Case Keyboard |
| `0xF782` | KreativeKorp K130 Keyboard |
| `0xF800` | MachiKania team MachiKania Gamepad |
| `0xFA00` | Sam MacLennan SFP Access Port |
| `0xFA11` | moonglow OpenXHC |
| `0xFA45` | Vekatech VK-RA4M2-FEMTO (Arduino Application) |
| `0xFA48` | Vekatech VK-RA4M2-FEMTO (Arduino Bootloader) |
| `0xFA4A` | Vekatech VK-RA4M2-NANO (Arduino Application) |
| `0xFA4B` | Vekatech VK-RA4M2-NANO (Arduino Bootloader) |
| `0xFA55` | TurtlPass Firmware |
| `0xFA57` | 3DRacers Pilot Board |
| `0xFA58` | 3DRacers Pilot Board (Bootloader) |
| `0xFA70` | Bytes of Serial Relacon USB Relay Controller |
| `0xFAB1` | PAP Mechatronic Technology LamDiNao |
| `0xFACA` | brunofreitas.com ControllaBLE |
| `0xFACE` | Protean Synth Craft |
| `0xFAD1` | Oxion 16nx |
| `0xFADA` | Music Thing Modular 8mu |
| `0xFADE` | Open Collector dude |
| `0xFAF0` | RAJlab RISCy Logger Pro |
| `0xFBA0` | studiohsoftware Wireless Preset Manager |
| `0xFC00` | Charlie Smurthwaite (catphish) OpenUAV |
| `0xFC30` | Gary Wong FPC-III |
| `0xFD42` | HXR.DK SAO Digital Multimeter |
| `0xFDFD` | Futility Designs Continuum Keyboard Firmware |
| `0xFE05` | Stardot Ultimate Electron Upgrade |
| `0xFE06` | Myelin POST Box |
| `0xFE07` | Myelin Arcflash |
| `0xFE80` | skji coquette |
| `0xFEE1` | ESSOTEC XENPAD One |
| `0xFEED` | ProgramGyar AVR-IR Sender |
| `0xFEFE` | Itamar Eliakim stm32f1 rosserial via USB VCP |
| `0xFF01` | digitarhythm TReK withTK |
| `0xFF02` | digitarhythm TReK G50 TIJUANA |
| `0xFF03` | digitarhythm TReK 64 AVAIN |
| `0xFF04` | digitarhythm TReK Lagoon |
| `0xFF05` | digitarhythm TReK Lagoon SP |
| `0xFF06` | digitarhythm TReK Periodt |
| `0xFF07` | digitarhythm TReK Amphibious |
| `0xFF08` | digitarhythm TReK GRAIN |
| `0xFF09` | digitarhythm TReK Rouge |
| `0xFF0A` | digitarhythm TReK Lettio |
| `0xFF40` | RF.Guru µPico |
| `0xFF50` | Adam Greig FFP iCE40 FPGA/Flash Programmer |
| `0xFF60` | BrainBoardz Neuron |
| `0xFF90` | ProtoCentral HealthyPi 5 |
| `0xFF91` | ProtoCentral HealthyPi 6 |
| `0xFFB0` | Yannick Richter OpenFFBoard |
| `0xFFCA` | hi0t fcart |
| `0xFFFE` | Kirill Kotyagin (R2AXZ) STM32 Blue Pill Serial Monster |
| `0xFFFF` | Life2Device Smart House |

