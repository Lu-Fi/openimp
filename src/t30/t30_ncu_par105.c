/*
 * T30 NCU (noise cancel unit) per-gain register download, vendor libimp 1.0.5
 * ncu_func_par_download (T30X stock library, gcc 5.4.0 build, md5 ad636e05, nm 0xd86d8,
 * 0x5b2c bytes; Lu-Fi/openimp #703).
 *
 * Mechanical transcription of the decompiled vendor function: the active parameter
 * set (one 0x760-byte day or night block of the 0xec8-byte /etc/sensor/ncu_<sensor>.bin of
 * the vendor 1.0.5 layout) is read through P(offset); every tuned value is a 9-point table
 * interpolated over the total gain (t30_ncu_lin = ncu_func_linear_insert, t30_ncu_nlin =
 * ncu_func_non_linear_insert), then packed into the NCU registers 0x13090180..0x400.
 * The register sequence is checked write by write against the vendor function running
 * under qemu-mipsel (tests/t30/ncu_test.c, tests/t30/fixtures/ncu105_*.txt).
 * Differences to the vendor: the debug logging (flag bits 1 and 2 of the set) is dropped,
 * and a zero divisor (vendor: break 7 trap) makes the function return -1.
 * Local names are the decompiler's; do not hand-edit, regenerate (tools/t30_ncu/gen_par105.py)
 * and re-check.
 */
#define T30_NCU_GENERATED
#include "t30_ncu_internal.h"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

int t30_ncu_par105_download(const uint8_t *pb, int32_t param_1)
{
  const uint8_t *puVar1;
  const uint8_t *puVar2;
  uint32_t uVar3;
  int32_t iVar4;
  uint32_t uVar5;
  uint32_t uVar6;
  uint32_t uVar7;
  uint32_t uVar8;
  uint32_t uVar9;
  uint32_t uVar10;
  uint32_t uVar11;
  uint32_t uVar12;
  uint32_t uVar13;
  uint32_t uVar14;
  uint32_t uVar15;
  uint32_t uVar16;
  uint32_t uVar17;
  uint32_t uVar18;
  uint32_t uVar19;
  uint32_t uVar20;
  uint32_t uVar21;
  uint32_t uVar22;
  uint32_t uVar23;
  uint32_t uVar24;
  uint32_t uVar25;
  uint32_t uVar26;
  uint32_t uVar27;
  uint32_t uVar28;
  uint32_t uVar29;
  uint32_t uVar30;
  uint32_t uVar31;
  uint32_t uVar32;
  uint32_t uVar33;
  uint32_t uVar34;
  uint32_t uVar35;
  uint32_t uVar36;
  uint32_t uVar37;
  uint32_t uVar38;
  uint32_t uVar39;
  uint32_t uVar40;
  uint32_t uVar41;
  uint32_t uVar42;
  uint32_t uVar43;
  uint32_t uVar44;
  uint32_t uVar45;
  uint32_t uVar46;
  uint32_t uVar47;
  uint32_t uVar48;
  uint32_t uVar49;
  uint32_t *puVar50;
  uint32_t uVar51;
  uint32_t uVar52;
  int32_t iVar53;
  uint32_t uVar54;
  uint32_t uVar55;
  uint32_t uVar56;
  uint32_t uVar57;
  uint32_t uVar58;
  uint32_t uVar59;
  uint32_t uVar60;
  uint32_t uVar61;
  uint32_t uVar62;
  uint32_t uVar63;
  uint32_t uVar64;
  uint32_t uVar65;
  uint32_t uVar66;
  uint32_t uVar67;
  uint32_t uVar68;
  uint32_t uVar69;
  uint32_t uVar70;
  uint32_t uVar71;
  uint32_t uVar72;
  uint32_t uVar73;
  uint32_t uVar74;
  uint32_t uVar75;
  int32_t iVar76;
  uint32_t uVar77;
  uint32_t uVar78;
  uint32_t uVar79;
  uint32_t uVar80;
  uint32_t uVar81;
  uint32_t uVar82;
  uint32_t uVar83;
  uint32_t uVar84;
  uint32_t uVar85;
  uint32_t uVar86;
  uint32_t uVar87;
  uint32_t uVar88;
  uint32_t uVar89;
  uint32_t uVar90;
  const uint8_t *puVar91;
  uint32_t uVar92;
  uint32_t uVar93;
  const uint8_t *puVar94;
  int32_t iVar95;
  uint32_t uVar96;
  uint32_t uVar97;
  const uint8_t *puVar98;
  uint32_t uVar99;
  uint32_t uVar100;
  uint32_t uVar101;
  uint32_t uVar102;
  uint32_t uVar103;
  void *pcVar104;
  uint32_t uVar105;
  uint32_t uVar106;

  uVar92 = P(0x30);
  uVar89 = P(0x4);
  uVar96 = P(0x2c);
  uVar97 = P(0);
  uVar3 = R(0x24);
  uVar89 = (uVar92 & 1) << 0xb | (uVar92 & 1) << 0xe | (uVar92 & 1) << 0xf | (uVar96 & 1) << 9 |
           uVar3 & 0xff | (uVar89 & 0xffff35) << 8;
  W(0xc,4);
  uVar3 = (uVar97 & 3) >> 1;
  if ((int)(uVar97 << 0x1b) < 0) {
    W(0x24,uVar89);
    if (-1 < (int)(uVar97 << 0x1c)) goto LAB_000e87d0;
  }
  else {
    if (-1 < (int)(uVar97 << 0x1c)) goto LAB_000e87d0;
  }
  W(4,2);
LAB_000e87d0:
  if ((int)(uVar97 << 0x1d) < 0) {
  }
  if ((int)(uVar97 << 0x1e) < 0) {
  }
  uVar51 = P(0x20);
  uVar60 = P(0xc);
  uVar62 = P(0x8);
  uVar59 = P(0x10);
  uVar58 = P(0x14);
  uVar56 = P(0x18);
  uVar54 = P(0x1c);
  uVar90 = P(0x24);
  uVar99 = P(0x58);
  uVar93 = P(0x28);
  uVar63 = P(0x5c);
  uVar64 = P(0x60);
  uVar65 = P(0x64);
  uVar57 = P(0x68);
  uVar55 = P(0x6c);
  uVar83 = P(0x70);
  uVar61 = P(0x74);
  uVar52 = P(0x78);
  uVar92 = LIN
                     (P(0x34),P(0x38),
                      P(0x3c),P(0x40),
                      P(0x44),P(0x48),
                      P(0x4c),P(0x50),
                      P(0x54),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,uVar51
                      ,uVar90,uVar93,param_1);
  uVar96 = LIN
                     (uVar99,uVar63,uVar64,uVar65,uVar57,uVar55,uVar83,uVar61,uVar52,uVar62,uVar60,
                      uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar97 = LIN
                     (P(0x7c),P(0x80),
                      P(0x84),P(0x88),
                      P(0x8c),P(0x90),
                      P(0x94),P(0x98),
                      P(0x9c),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,uVar51
                      ,uVar90,uVar93,param_1);
  uVar73 = P(0xc4);
  uVar77 = P(0xc8);
  uVar84 = P(0xcc);
  uVar100 = P(0xd0);
  uVar61 = P(0xd4);
  uVar55 = P(0xd8);
  uVar63 = P(0xdc);
  uVar64 = P(0xe0);
  uVar68 = P(0xe4);
  uVar65 = P(0xe8);
  uVar83 = P(0xec);
  uVar99 = P(0xf0);
  uVar66 = P(0xf4);
  uVar57 = P(0xf8);
  uVar78 = P(0xfc);
  uVar85 = P(0x100);
  uVar101 = P(0x104);
  uVar52 = P(0x108);
  uVar5 = LIN
                    (P(0xa0),P(0xa4),
                     P(0xa8),P(0xac),
                     P(0xb0),P(0xb4),
                     P(0xb8),P(0xbc),
                     P(0xc0),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,uVar51,
                     uVar90,uVar93,param_1);
  uVar6 = LIN
                    (uVar73,uVar77,uVar84,uVar100,uVar61,uVar55,uVar63,uVar64,uVar68,uVar62,uVar60,
                     uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar7 = LIN
                    (uVar65,uVar83,uVar99,uVar66,uVar57,uVar78,uVar85,uVar101,uVar52,uVar62,uVar60,
                     uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar61 = P(0x130);
  uVar63 = P(0x134);
  uVar64 = P(0x138);
  uVar65 = P(0x13c);
  uVar57 = P(0x140);
  uVar55 = P(0x144);
  uVar83 = P(0x148);
  uVar99 = P(0x14c);
  uVar52 = P(0x150);
  uVar8 = LIN
                    (P(0x10c),P(0x110),
                     P(0x114),P(0x118),
                     P(0x11c),P(0x120),
                     P(0x124),P(0x128),
                     P(0x12c),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,uVar51,
                     uVar90,uVar93,param_1);
  uVar9 = LIN
                    (uVar61,uVar63,uVar64,uVar65,uVar57,uVar55,uVar83,uVar99,uVar52,uVar62,uVar60,
                     uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar61 = P(0x178);
  uVar63 = P(0x17c);
  uVar64 = P(0x180);
  uVar65 = P(0x184);
  uVar57 = P(0x188);
  uVar55 = P(0x18c);
  uVar83 = P(0x190);
  uVar99 = P(0x194);
  uVar52 = P(0x198);
  uVar10 = LIN
                     (P(0x154),P(0x158),
                      P(0x15c),P(0x160),
                      P(0x164),P(0x168),
                      P(0x16c),P(0x170),
                      P(0x174),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar11 = LIN
                     (uVar61,uVar63,uVar64,uVar65,uVar57,uVar55,uVar83,uVar99,uVar52,uVar62,uVar60,
                      uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar12 = LIN
                     (P(0x19c),P(0x1a0),
                      P(0x1a4),P(0x1a8),
                      P(0x1ac),P(0x1b0),
                      P(0x1b4),P(0x1b8),
                      P(0x1bc),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar13 = LIN
                     (P(0x1c0),P(0x1c4),
                      P(0x1c8),P(0x1cc),
                      P(0x1d0),P(0x1d4),
                      P(0x1d8),P(0x1dc),
                      P(0x1e0),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar14 = LIN
                     (P(0x1e4),P(0x1e8),
                      P(0x1ec),P(0x1f0),
                      P(0x1f4),P(0x1f8),
                      P(0x1fc),P(0x200),
                      P(0x204),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar89 = NLIN
                     (P(0x208),P(0x20c),
                      P(0x210),P(0x214),
                      P(0x218),P(0x21c),
                      P(0x220),P(0x224),
                      P(0x228),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar15 = LIN
                     (P(0x250),P(0x254),
                      P(0x258),P(0x25c),
                      P(0x260),P(0x264),
                      P(0x268),P(0x26c),
                      P(0x270),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar16 = LIN
                     (P(0x22c),P(0x230),
                      P(0x234),P(0x238),
                      P(0x23c),P(0x240),
                      P(0x244),P(0x248),
                      P(0x24c),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar17 = LIN
                     (P(0x274),P(0x278),
                      P(0x27c),P(0x280),
                      P(0x284),P(0x288),
                      P(0x28c),P(0x290),
                      P(0x294),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  iVar4 = LIN
                    (P(0x298),P(0x29c),
                     P(0x2a0),P(0x2a4),
                     P(0x2a8),P(0x2ac),
                     P(0x2b0),P(0x2b4),
                     P(0x2b8),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,uVar51
                     ,uVar90,uVar93,param_1);
  uVar18 = LIN
                     (P(0x2bc),P(0x2c0),
                      P(0x2c4),P(0x2c8),
                      P(0x2cc),P(0x2d0),
                      P(0x2d4),P(0x2d8),
                      P(0x2dc),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar19 = LIN
                     (P(0x2e0),P(0x2e4),
                      P(0x2e8),P(0x2ec),
                      P(0x2f0),P(0x2f4),
                      P(0x2f8),P(0x2fc),
                      P(0x300),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar20 = LIN
                     (P(0x304),P(0x308),
                      P(0x30c),P(0x310),
                      P(0x314),P(0x318),
                      P(0x31c),P(0x320),
                      P(0x324),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar21 = LIN
                     (P(0x328),P(0x32c),
                      P(0x330),P(0x334),
                      P(0x338),P(0x33c),
                      P(0x340),P(0x344),
                      P(0x348),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar22 = LIN
                     (P(0x34c),P(0x350),
                      P(0x354),P(0x358),
                      P(0x35c),P(0x360),
                      P(0x364),P(0x368),
                      P(0x36c),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar23 = NLIN
                     (P(0x370),P(0x374),
                      P(0x378),P(0x37c),
                      P(0x380),P(0x384),
                      P(0x388),P(0x38c),
                      P(0x390),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar99 = P(0x3b8);
  uVar101 = P(0x3bc);
  uVar69 = P(0x3c0);
  uVar85 = P(0x3c4);
  uVar65 = P(0x3c8);
  uVar74 = P(0x3cc);
  uVar63 = P(0x3d0);
  uVar52 = P(0x3d4);
  uVar86 = P(0x3d8);
  uVar79 = P(0x3dc);
  uVar78 = P(0x3e0);
  uVar80 = P(0x3e4);
  uVar66 = P(0x3e8);
  uVar102 = P(0x3ec);
  uVar105 = P(0x3f0);
  uVar55 = P(0x3f4);
  uVar75 = P(0x3f8);
  uVar70 = P(0x3fc);
  uVar67 = P(0x400);
  uVar81 = P(0x404);
  uVar73 = P(0x408);
  uVar87 = P(0x40c);
  uVar64 = P(0x410);
  uVar57 = P(0x414);
  uVar82 = P(0x418);
  uVar88 = P(0x41c);
  uVar103 = P(0x420);
  uVar68 = P(0x424);
  uVar83 = P(0x428);
  uVar106 = P(0x42c);
  uVar77 = P(0x430);
  uVar84 = P(0x434);
  uVar100 = P(0x438);
  uVar71 = P(0x43c);
  uVar72 = P(0x440);
  uVar61 = P(0x444);
  uVar24 = LIN
                     (P(0x394),P(0x398),
                      P(0x39c),P(0x3a0),
                      P(0x3a4),P(0x3a8),
                      P(0x3ac),P(0x3b0),
                      P(0x3b4),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar25 = LIN
                     (uVar99,uVar101,uVar69,uVar85,uVar65,uVar74,uVar63,uVar52,uVar86,uVar62,uVar60,
                      uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar26 = LIN
                     (uVar79,uVar78,uVar80,uVar66,uVar102,uVar105,uVar55,uVar75,uVar70,uVar62,uVar60
                      ,uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar27 = LIN
                     (uVar67,uVar81,uVar73,uVar87,uVar64,uVar57,uVar82,uVar88,uVar103,uVar62,uVar60,
                      uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar28 = LIN
                     (uVar68,uVar83,uVar106,uVar77,uVar84,uVar100,uVar71,uVar72,uVar61,uVar62,uVar60
                      ,uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar29 = LIN
                     (P(0x448),P(0x44c),
                      P(0x450),P(0x454),
                      P(0x458),P(0x45c),
                      P(0x460),P(0x464),
                      P(0x468),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar63 = P(0x490);
  uVar64 = P(0x494);
  uVar65 = P(0x498);
  uVar83 = P(0x49c);
  uVar99 = P(0x4a0);
  uVar61 = P(0x4a4);
  uVar55 = P(0x4a8);
  uVar52 = P(0x4ac);
  uVar57 = P(0x4b0);
  uVar30 = LIN
                     (P(0x46c),P(0x470),
                      P(0x474),P(0x478),
                      P(0x47c),P(0x480),
                      P(0x484),P(0x488),
                      P(0x48c),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar31 = LIN
                     (uVar63,uVar64,uVar65,uVar83,uVar99,uVar61,uVar55,uVar52,uVar57,uVar62,uVar60,
                      uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar32 = LIN
                     (P(0x4b4),P(0x4b8),
                      P(0x4bc),P(0x4c0),
                      P(0x4c4),P(0x4c8),
                      P(0x4cc),P(0x4d0),
                      P(0x4d4),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar63 = P(0x4fc);
  uVar64 = P(0x500);
  uVar65 = P(0x504);
  uVar83 = P(0x508);
  uVar99 = P(0x50c);
  uVar61 = P(0x510);
  uVar55 = P(0x514);
  uVar52 = P(0x518);
  uVar57 = P(0x51c);
  uVar33 = LIN
                     (P(0x4d8),P(0x4dc),
                      P(0x4e0),P(0x4e4),
                      P(0x4e8),P(0x4ec),
                      P(0x4f0),P(0x4f4),
                      P(0x4f8),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar34 = LIN
                     (uVar63,uVar64,uVar65,uVar83,uVar99,uVar61,uVar55,uVar52,uVar57,uVar62,uVar60,
                      uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar35 = LIN
                     (P(0x520),P(0x524),
                      P(0x528),P(0x52c),
                      P(0x530),P(0x534),
                      P(0x538),P(0x53c),
                      P(0x540),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar36 = LIN
                     (P(0x544),P(0x548),
                      P(0x54c),P(0x550),
                      P(0x554),P(0x558),
                      P(0x55c),P(0x560),
                      P(0x564),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar37 = LIN
                     (P(0x58c),P(0x590),
                      P(0x594),P(0x598),
                      P(0x59c),P(0x5a0),
                      P(0x5a4),P(0x5a8),
                      P(0x5ac),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar38 = LIN
                     (P(0x568),P(0x56c),
                      P(0x570),P(0x574),
                      P(0x578),P(0x57c),
                      P(0x580),P(0x584),
                      P(0x588),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar39 = LIN
                     (P(0x5b0),P(0x5b4),
                      P(0x5b8),P(0x5bc),
                      P(0x5c0),P(0x5c4),
                      P(0x5c8),P(0x5cc),
                      P(0x5d0),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar40 = LIN
                     (P(0x5d4),P(0x5d8),
                      P(0x5dc),P(0x5e0),
                      P(0x5e4),P(0x5e8),
                      P(0x5ec),P(0x5f0),
                      P(0x5f4),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar41 = LIN
                     (P(0x5f8),P(0x5fc),
                      P(0x600),P(0x604),
                      P(0x608),P(0x60c),
                      P(0x610),P(0x614),
                      P(0x618),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar42 = LIN
                     (P(0x61c),P(0x620),
                      P(0x624),P(0x628),
                      P(0x62c),P(0x630),
                      P(0x634),P(0x638),
                      P(0x63c),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar99 = P(0x688);
  uVar101 = P(0x68c);
  uVar69 = P(0x690);
  uVar85 = P(0x694);
  uVar65 = P(0x698);
  uVar74 = P(0x69c);
  uVar63 = P(0x6a0);
  uVar52 = P(0x6a4);
  uVar86 = P(0x6a8);
  uVar79 = P(0x6ac);
  uVar78 = P(0x6b0);
  uVar80 = P(0x6b4);
  uVar66 = P(0x6b8);
  uVar102 = P(0x6bc);
  uVar105 = P(0x6c0);
  uVar55 = P(0x6c4);
  uVar75 = P(0x6c8);
  uVar70 = P(0x6cc);
  uVar67 = P(0x6d0);
  uVar81 = P(0x6d4);
  uVar73 = P(0x6d8);
  uVar87 = P(0x6dc);
  uVar64 = P(0x6e0);
  uVar57 = P(0x6e4);
  uVar82 = P(0x6e8);
  uVar88 = P(0x6ec);
  uVar103 = P(0x6f0);
  uVar68 = P(0x6f4);
  uVar83 = P(0x6f8);
  uVar106 = P(0x6fc);
  uVar77 = P(0x700);
  uVar84 = P(0x704);
  uVar100 = P(0x708);
  uVar71 = P(0x70c);
  uVar72 = P(0x710);
  uVar61 = P(0x714);
  uVar43 = LIN
                     (P(0x664),P(0x668),
                      P(0x66c),P(0x670),
                      P(0x674),P(0x678),
                      P(0x67c),P(0x680),
                      P(0x684),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar44 = LIN
                     (uVar99,uVar101,uVar69,uVar85,uVar65,uVar74,uVar63,uVar52,uVar86,uVar62,uVar60,
                      uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar45 = LIN
                     (uVar79,uVar78,uVar80,uVar66,uVar102,uVar105,uVar55,uVar75,uVar70,uVar62,uVar60
                      ,uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar46 = LIN
                     (uVar67,uVar81,uVar73,uVar87,uVar64,uVar57,uVar82,uVar88,uVar103,uVar62,uVar60,
                      uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar47 = LIN
                     (uVar68,uVar83,uVar106,uVar77,uVar84,uVar100,uVar71,uVar72,uVar61,uVar62,uVar60
                      ,uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  uVar83 = P(0x740);
  uVar99 = P(0x73c);
  uVar52 = P(0x744);
  uVar55 = P(0x748);
  uVar57 = P(0x74c);
  uVar61 = P(0x750);
  uVar63 = P(0x754);
  uVar64 = P(0x758);
  uVar65 = P(0x75c);
  uVar48 = LIN
                     (P(0x718),P(0x71c),
                      P(0x720),P(0x724),
                      P(0x728),P(0x72c),
                      P(0x730),P(0x734),
                      P(0x738),uVar62,uVar60,uVar59,uVar58,uVar56,uVar54,
                      uVar51,uVar90,uVar93,param_1);
  uVar49 = LIN
                     (uVar99,uVar83,uVar52,uVar55,uVar57,uVar61,uVar63,uVar64,uVar65,uVar62,uVar60,
                      uVar59,uVar58,uVar56,uVar54,uVar51,uVar90,uVar93,param_1);
  iVar53 = (int)uVar89 / 2;
  if (uVar3 != 0) {
  }
  W(0x180,0);
  W(0x184,0);
  W(0x188,0);
  W(0x18c,0);
  W(400,0);
  W(0x194,0);
  W(0x198,0);
  W(0x19c,0);
  W(0x1a0,0);
  W(0x1a4,0);
  W(0x1a8,0);
  W(0x1ac,0);
  W(0x1b0,0);
  W(0x1d0,(uVar23 & 1) << 10 | uVar14 & 3 | 0x200);
  W(0x1d4,(uVar15 & 0x1ff) << 10 | uVar15 & 0x1ff | 0x1ff00000);
  uVar21 = uVar21 & 0x1ff;
  W(0x1d8,0x7fdff);
  uVar3 = uVar21 << 0x10;
  W(0x1dc,uVar22 & 0x1ff | uVar3);
  W(0x1e0,uVar21 | uVar3);
  W(0x1e4,0);
  W(0x1e8,0);
  W(0x1ec,(uVar27 & 0x3f) << 0x18 | (uVar26 & 0x3f) << 0x10 | uVar24 & 0x3f |
                            (uVar25 & 0x3f) << 8);
  W(0x1f0,uVar28 & 0x3f);
  W(500,0);
  W(0x1f8,0);
  W(0x1fc,0xffff);
  W(0x200,0x147f);
  W(0x204,uVar21);
  W(0x208,(uVar92 & 0x3f) << 0x18 | (uVar92 & 0x3f) << 0x10 | uVar96 & 0x3ff);
  W(0x220,(uVar5 & 3) << 0x1d | (uVar97 & 0x1ff) << 0x14 | uVar6 & 0x1ff |
                            (uVar7 & 0x1ff) << 10);
  W(0x224,0);
  W(0x228,0);
  W(0x22c,0);
  W(0x230,0);
  W(0x234,0);
  W(0x238,0);
  W(0x23c,0);
  W(0x240,0);
  W(0x244,0);
  W(0x248,0);
  W(0x24c,0);
  W(0x250,0);
  W(0x254,(uVar9 & 7) << 0x18 | (uVar8 & 0xff) << 0x10 | uVar8 & 0xff |
                            (uVar9 & 7) << 8);
  W(600,0);
  W(0x25c,0);
  W(0x260,0);
  W(0x264,0);
  W(0x268,0);
  W(0x26c,0);
  W(0x270,0);
  W(0x274,0);
  W(0x278,0);
  W(0x27c,0);
  W(0x280,0);
  W(0x284,0);
  W(0x288,(uVar11 & 7) << 0x18 | (uVar10 & 0xff) << 0x10 | uVar10 & 0xff |
                            (uVar11 & 7) << 8);
  W(0x2a0,(uVar18 & 0xff) << 0xc | (uVar20 & 0x1ff) << 0x14 | uVar19 & 0x1f |
                            (uVar12 & 1) << 9 | (uVar16 & 1) << 8);
  W(0x2a4,uVar3 | uVar20 & 0x1ff);
  iVar76 = ncu_img_h;
  iVar95 = ncu_img_w;
  uVar3 = ((int)uVar89 ? iVar76 / (int)uVar89 : 0);
  if (uVar89 == 0) {
    return -1;
  }
  uVar92 = uVar89 + 0xf;
  if ((int)uVar89 >= 0) {
    uVar92 = uVar89;
  }
  uVar96 = (uVar41 & 0x1ff) << 0x10;
  if (((int)uVar89 ? iVar76 % (int)uVar89 : 0) != 0) {
    uVar3 = uVar3 + 1;
  }
  uVar97 = ((int)uVar89 ? iVar95 / (int)uVar89 : 0);
  if (uVar89 == 0) {
    return -1;
  }
  if (((int)uVar89 ? iVar95 % (int)uVar89 : 0) != 0) {
    uVar97 = uVar97 + 1;
  }
  W(0x2a8,uVar14 & 3);
  uVar92 = (int)uVar92 >> 4 & 0xff;
  W(0x2ac,uVar92 << 0x10 | uVar92);
  W(0x2c4,(uVar97 & 0xff) << 0x10 | uVar3 & 0x3ff);
  W(0x2b0,uVar13 & 0x1ff);
  uVar92 = (uVar17 & 0x1ff) << 0x10 | uVar17 & 0x1ff;
  W(0x2b4,uVar92);
  W(0x2b8,iVar4);
  uVar3 = (int)uVar89 % 0x10 & 0xf;
  W(0x2c0,uVar3 << 8 | uVar3);
  W(700,0xff);
  W(0x2d0,uVar29 & 0xff);
  W(0x2d4,0x10);
  W(0x2d8,0);
  W(0x2dc,0);
  W(0x300,0);
  W(0x304,0);
  W(0x308,0);
  W(0x30c,0);
  W(0x310,0);
  W(0x314,0);
  W(0x318,0);
  W(0x31c,0);
  W(800,0);
  W(0x324,0);
  W(0x328,0);
  W(0x32c,0);
  W(0x340,uVar96 | 1);
  W(0x344,0xff00ff);
  W(0x348,(uVar37 & 0x1ff) << 0x10 | uVar37 & 0x1ff);
  W(0x34c,0x1ff01ff);
  W(0x350,uVar42 & 0x1ff | uVar96);
  W(0x354,uVar41 & 0x1ff);
  W(0x358,0);
  W(0x35c,0);
  W(0x360,(uVar46 & 0x3f) << 0x18 | (uVar45 & 0x3f) << 0x10 | uVar43 & 0x3f |
                            (uVar44 & 0x3f) << 8);
  W(0x364,uVar47 & 0x3f);
  W(0x368,0);
  W(0x36c,0);
  W(0x370,(uVar30 & 0x3f) << 0x18 | (uVar30 & 0x3f) << 0x10 | uVar31 & 0x3ff);
  W(0x390,uVar32 & 0x1ff);
  W(0x394,0);
  W(0x398,0);
  W(0x39c,0);
  W(0x3a0,0);
  W(0x3a4,0);
  W(0x3a8,0);
  W(0x3ac,uVar33 & 0x1ff);
  W(0x3b0,0xff00ff);
  W(0x3b4,0);
  W(0x3b8,0);
  W(0x3bc,0);
  W(0x3c0,0);
  W(0x3c4,0);
  W(0x3c8,0);
  W(0x3cc,uVar34 & 0x1ff);
  W(0x3d0,0xff00ff);
  W(0x3e0,(uVar39 & 0xff) << 0xc | (uVar40 & 0x1ff) << 0x14 | uVar38 & 1 |
                            (uVar19 & 0x1f) << 4 | (uVar35 & 1) << 1);
  W(0x3e4,uVar96 | uVar40 & 0x1ff);
  iVar76 = ncu_img_w / 2;
  uVar3 = (iVar53 ? iVar76 / iVar53 : 0);
  if (iVar53 == 0) {
    return -1;
  }
  iVar95 = ncu_img_h / 2;
  if ((int)uVar89 < 0) {
    uVar89 = uVar89 + 0x1f;
  }
  uVar89 = (uVar89 & 0x1fff) >> 5;
  if ((iVar53 ? iVar76 % iVar53 : 0) != 0) {
    uVar3 = uVar3 + 1;
  }
  uVar96 = (iVar53 ? iVar95 / iVar53 : 0);
  if (iVar53 == 0) {
    return -1;
  }
  if ((iVar53 ? iVar95 % iVar53 : 0) != 0) {
    uVar96 = uVar96 + 1;
  }
  W(1000,uVar89 << 0x10 | uVar89);
  W(0x3ec,uVar36 & 0x1ff);
  W(0x3f0,uVar92);
  W(0x3f4,iVar4 / 2 & 0x1fffff);
  uVar89 = iVar53 % 0x10 & 0xf;
  W(0x3fc,uVar89 << 8 | uVar89);
  W(0x400,(uVar3 & 0xff) << 0x10 | uVar96 & 0x3ff);
  W(0x3f8,(uVar49 & 3) << 0x10 | uVar48 & 0xff | 0x8000);
  W(0xc,5);
  return 0;

}
