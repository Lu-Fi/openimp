/*
 * T30 NCU (noise cancel unit) per-gain register download, vendor libimp 1.0.4
 * ncu_func_par_download (stock ABUS firmware libimp 1.0.4, md5 962be4bc, nm 0xd94b0,
 * 0x9268 bytes; Lu-Fi/openimp #701).
 *
 * Mechanical transcription of the decompiled vendor function: the active parameter
 * set (one 0xaa0-byte day or night block of /etc/sensor/ncu_<sensor>.bin) is read
 * through P(offset); every tuned value is a 9-point table interpolated over the
 * total gain (t30_ncu_lin = ncu_func_linear_insert, t30_ncu_nlin =
 * ncu_func_non_linear_insert), then packed into the NCU registers 0x13090180..0x400.
 * The register sequence is checked write by write against the vendor function
 * running under qemu-mipsel (tests/t30/ncu_test.c, tests/t30/fixtures/ncu104_*.txt).
 * Local names are the decompiler's; do not hand-edit, regenerate and re-check.
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

int t30_ncu_par104_download(const uint8_t *pb, int32_t param_1)
{
  int32_t iVar1;
  int32_t iVar2;
  int32_t iVar3;
  int32_t iVar4;
  int32_t iVar5;
  int bVar6;
  const uint8_t *puVar7;
  const uint8_t *puVar8;
  uint32_t *puVar9;
  uint32_t uVar10;
  int32_t iVar11;
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
  uint32_t uVar50;
  uint32_t uVar51;
  uint32_t uVar52;
  uint32_t uVar53;
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
  int32_t iVar67;
  int32_t iVar68;
  int32_t iVar69;
  int32_t iVar70;
  uint32_t uVar71;
  uint32_t uVar72;
  uint32_t uVar73;
  uint32_t uVar74;
  uint32_t uVar75;
  uint32_t uVar76;
  uint32_t uVar77;
  uint32_t uVar78;
  uint32_t uVar79;
  uint32_t uVar80;
  uint32_t uVar81;
  int32_t iVar82;
  uint32_t uVar83;
  uint32_t uVar84;
  uint32_t uVar85;
  uint32_t uVar86;
  uint32_t uVar87;
  uint32_t uVar88;
  uint32_t uVar89;
  uint32_t uVar90;
  uint32_t uVar91;
  uint32_t uVar92;
  uint32_t uVar93;
  int32_t iVar94;
  uint32_t uVar95;
  uint32_t uVar96;
  uint32_t uVar97;
  uint32_t uVar98;
  int32_t iVar99;
  int32_t iVar100;
  uint32_t uVar101;
  uint32_t uVar102;
  uint32_t uVar103;
  uint32_t uVar104;
  uint32_t uVar105;
  uint32_t uVar106;
  uint32_t uVar107;
  uint32_t uVar108;
  uint32_t uVar109;
  uint32_t uVar110;
  uint32_t uVar111;
  uint32_t uVar112;
  uint32_t uVar113;
  uint32_t uVar114;
  uint32_t uVar115;
  uint32_t uVar116;
  uint32_t uVar117;
  uint32_t uVar118;
  uint32_t uVar119;
  uint32_t uVar120;
  uint32_t uVar121;
  int32_t iVar122;
  uint32_t uVar123;
  uint32_t uVar124;
  uint32_t uVar125;
  uint32_t uVar126;
  uint32_t uVar127;
  uint32_t uVar128;
  uint32_t uVar129;
  uint32_t uVar130;
  uint32_t uVar131;
  uint32_t uVar132;
  uint32_t uVar133;
  uint32_t uVar134;
  const uint8_t *puVar135;
  uint32_t uVar136;
  const uint8_t *puVar137;
  uint32_t uVar138;
  const uint8_t *puVar139;
  uint32_t uVar140;
  uint32_t uVar141;
  int32_t iVar142;
  uint32_t uVar143;
  uint32_t uVar144;
  uint32_t uVar145;
  uint32_t uVar146;
  uint32_t uVar147;
  uint32_t uVar148;
  uint32_t uVar149;
  uint32_t uVar150;
  uint32_t uVar151;
  uint32_t uVar152;
  uint32_t uVar153;
  uint32_t uVar154;
  uint32_t uVar155;
  uint32_t uVar156;
  uint32_t uVar157;
  uint32_t uVar158;
  uint32_t uVar159;
  uint32_t uVar160;
  uint32_t uVar161;
  uint32_t uVar162;
  uint32_t uVar163;
  uint32_t uVar164;
  uint32_t uVar165;
  uint32_t uVar166;
  uint32_t uVar167;
  uint32_t uVar168;
  uint32_t uVar169;
  void *pcVar170;
  uint32_t uVar171;
  uint32_t uVar172;
  uint32_t uVar173;
  uint32_t uVar174;
  uint32_t uVar175;
  double dVar176;
  uint32_t uVar177;
  uint32_t uVar178;
  uint32_t uVar179;
  uint32_t uVar180;
  uint32_t uVar181;
  uint32_t uVar182;
  uint32_t uVar183;
  uint32_t uVar184;
  uint32_t local_330;
  uint32_t local_320;
  int32_t local_31c;
  uint32_t local_318;
  int32_t local_310;
  uint32_t local_308;
  uint32_t local_304;
  uint32_t local_300;
  uint32_t local_2f8;
  uint32_t local_2f4;
  uint32_t local_2f0;
  int32_t local_2ec;
  uint32_t local_2e8;
  int32_t local_2e4;
  int32_t local_2e0;
  int32_t local_2dc;
  int32_t local_2d8;
  uint32_t local_2d4;
  uint32_t local_2d0;
  uint32_t local_2cc;
  uint32_t local_2c8;
  uint32_t local_2bc;
  uint32_t local_2b8;
  uint32_t local_2b4;
  uint32_t local_2b0;
  uint32_t local_2ac;
  uint32_t local_2a8;
  uint32_t local_2a4;
  uint32_t local_2a0;
  uint32_t local_29c;
  uint32_t local_298;
  uint32_t local_294;
  uint32_t local_290;
  uint32_t local_28c;
  uint32_t local_288;
  uint32_t local_284;
  uint32_t local_280;
  uint32_t local_264;
  uint32_t local_260;
  uint32_t local_25c;
  uint32_t local_250;
  uint32_t local_238;
  int32_t local_234;
  uint32_t local_220;
  uint32_t local_21c;
  uint32_t local_208;
  uint32_t local_1a0;
  uint32_t local_19c;
  uint32_t local_198;
  uint32_t local_170;
  uint32_t local_e0;
  uint32_t local_dc;
  uint32_t local_d8;
  uint32_t local_d4;
  uint32_t local_d0;
  uint32_t local_cc;
  uint32_t local_c8;
  uint32_t local_c4;
  uint32_t local_c0;
  uint32_t local_bc;
  uint32_t local_b8;
  uint32_t local_b4;
  uint32_t local_b0;
  uint32_t local_ac;
  uint32_t local_a8;
  uint32_t local_a4;
  uint32_t local_a0;
  uint32_t local_9c;
  uint32_t local_98;
  uint32_t local_94;
  uint32_t local_90;
  uint32_t local_8c;
  uint32_t local_88;
  uint32_t local_84;
  uint32_t local_80;
  uint32_t local_7c;
  uint32_t local_78;
  uint32_t local_74;
  int32_t local_6c;
  uint32_t local_68;
  uint32_t local_64;
  uint32_t local_60;
  uint32_t local_5c;
  uint32_t local_58;

  uVar65 = P(0x4);
  uVar130 = P(0x5a4);
  uVar126 = P(0);
  uVar133 = P(0x2c);
  uVar10 = R(0x24);
  uVar130 = uVar10 & 0xff | (uVar65 & 0xffff35) << 8 |
            (uVar130 & 1) << 0xf | (uVar130 & 1) << 0xb | (uVar130 & 1) << 0xe | (uVar133 & 1) << 9;
  W(0xc,4);
  uVar10 = (uVar126 & 3) >> 1;
  if ((int)(uVar126 << 0x1b) < 0) {
    W(0x24,uVar130);
  }
  if ((int)(uVar126 << 0x1c) < 0) {
    W(4,2);
  }
  if ((int)(uVar126 << 0x1d) < 0) {
  }
  if (uVar10 != 0) {
  }
  uVar74 = P(0x8);
  uVar83 = P(0xc);
  uVar93 = P(0x30);
  uVar140 = P(0x10);
  uVar127 = P(0x14);
  uVar171 = P(0x18);
  uVar138 = P(0x1c);
  uVar136 = P(0x20);
  uVar134 = P(0x24);
  uVar131 = P(0x28);
  iVar100 = P(0x34);
  uVar105 = P(0x38);
  uVar110 = P(0x3c);
  uVar119 = P(0x40);
  local_234 = P(0x44);
  uVar84 = P(0x48);
  iVar94 = P(0x4c);
  uVar130 = LIN
                      (P(0x50),P(0x54),
                       P(0x58),P(0x5c),
                       P(0x60),P(0x64),
                       P(0x68),P(0x6c),
                       P(0x70),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                       uVar136,uVar134,uVar131,param_1);
  uVar85 = P(0x74);
  uVar95 = P(0x78);
  uVar101 = P(0x7c);
  uVar106 = P(0x80);
  uVar111 = P(0x84);
  uVar120 = P(0x88);
  uVar75 = P(0x8c);
  uVar86 = P(0x90);
  uVar96 = P(0x94);
  uVar133 = LIN
                      (P(0x98),P(0x9c),
                       P(0xa0),P(0xa4),
                       P(0xa8),P(0xac),
                       P(0xb0),P(0xb4),
                       P(0xb8),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                       uVar136,uVar134,uVar131,param_1);
  uVar12 = LIN
                     (P(0xbc),P(0xc0),
                      P(0xc4),P(0xc8),
                      P(0xcc),P(0xd0),
                      P(0xd4),P(0xd8),
                      P(0xdc),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar13 = LIN
                     (P(0xe0),P(0xe4),
                      P(0xe8),P(0xec),
                      P(0xf0),P(0xf4),
                      P(0xf8),P(0xfc),
                      P(0x100),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  local_2e4 = P(0x104);
  uVar112 = P(0x108);
  local_2e8 = LIN
                        (P(0x10c),P(0x110),
                         P(0x114),P(0x118),
                         P(0x11c),P(0x120),
                         P(0x124),P(0x128),
                         P(0x12c),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138
                         ,uVar136,uVar134,uVar131,param_1);
  uVar14 = LIN
                     (P(0x130),P(0x134),
                      P(0x138),P(0x13c),
                      P(0x140),P(0x144),
                      P(0x148),P(0x14c),
                      P(0x150),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  local_2e0 = P(0x154);
  uVar113 = P(0x158);
  local_264 = LIN
                        (P(0x15c),P(0x160),
                         P(0x164),P(0x168),
                         P(0x16c),P(0x170),
                         P(0x174),P(0x178),
                         P(0x17c),uVar74,uVar83,uVar140,uVar127,uVar171,
                         uVar138,uVar136,uVar134,uVar131,param_1);
  uVar15 = LIN
                     (P(0x180),P(0x184),
                      P(0x188),P(0x18c),
                      P(0x190),P(0x194),
                      P(0x198),P(0x19c),
                      P(0x1a0),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  local_2dc = P(0x1a4);
  uVar114 = P(0x1a8);
  uVar16 = LIN
                     (P(0x1ac),P(0x1b0),
                      P(0x1b4),P(0x1b8),
                      P(0x1bc),P(0x1c0),
                      P(0x1c4),P(0x1c8),
                      P(0x1cc),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar17 = LIN
                     (P(0x1d0),P(0x1d4),
                      P(0x1d8),P(0x1dc),
                      P(0x1e0),P(0x1e4),
                      P(0x1e8),P(0x1ec),
                      P(0x1f0),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  local_2d8 = P(0x1f4);
  uVar115 = P(0x1f8);
  uVar18 = LIN
                     (P(0x1fc),P(0x200),
                      P(0x204),P(0x208),
                      P(0x20c),P(0x210),
                      P(0x214),P(0x218),
                      P(0x21c),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar19 = LIN
                     (P(0x220),P(0x224),
                      P(0x228),P(0x22c),
                      P(0x230),P(0x234),
                      P(0x238),P(0x23c),
                      P(0x240),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar66 = P(0x28c);
  uVar143 = P(0x290);
  uVar141 = P(0x268);
  uVar124 = P(0x26c);
  uVar121 = P(0x270);
  uVar116 = P(0x274);
  uVar107 = P(0x278);
  uVar102 = P(0x27c);
  uVar97 = P(0x280);
  uVar87 = P(0x284);
  uVar76 = P(0x288);
  uVar144 = P(0x294);
  uVar145 = P(0x298);
  uVar146 = P(0x29c);
  uVar147 = P(0x2a0);
  uVar148 = P(0x2a4);
  uVar149 = P(0x2a8);
  uVar150 = P(0x2ac);
  uVar20 = LIN
                     (P(0x244),P(0x248),
                      P(0x24c),P(0x250),
                      P(0x254),P(0x258),
                      P(0x25c),P(0x260),
                      P(0x264),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar21 = LIN
                     (uVar141,uVar124,uVar121,uVar116,uVar107,uVar102,uVar97,uVar87,uVar76,uVar74,
                      uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar22 = LIN
                     (uVar66,uVar143,uVar144,uVar145,uVar146,uVar147,uVar148,uVar149,uVar150,uVar74,
                      uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar23 = NLIN
                     (P(0x2b0),P(0x2b4),
                      P(0x2b8),P(0x2bc),
                      P(0x2c0),P(0x2c4),
                      P(0x2c8),P(0x2cc),
                      P(0x2d0),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar24 = LIN
                     (P(0x2d4),P(0x2d8),
                      P(0x2dc),P(0x2e0),
                      P(0x2e4),P(0x2e8),
                      P(0x2ec),P(0x2f0),
                      P(0x2f4),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar25 = NLIN
                     (P(0x2f8),P(0x2fc),
                      P(0x300),P(0x304),
                      P(0x308),P(0x30c),
                      P(0x310),P(0x314),
                      P(0x318),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar26 = LIN
                     (P(0x31c),P(0x320),
                      P(0x324),P(0x328),
                      P(0x32c),P(0x330),
                      P(0x334),P(0x338),
                      P(0x33c),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar76 = P(0x364);
  uVar143 = P(0x36c);
  uVar66 = P(0x368);
  uVar144 = P(0x370);
  uVar145 = P(0x374);
  uVar87 = P(0x378);
  uVar97 = P(0x37c);
  uVar146 = P(0x388);
  uVar102 = P(0x380);
  uVar107 = P(0x384);
  uVar147 = P(0x38c);
  uVar148 = P(0x390);
  uVar149 = P(0x394);
  uVar150 = P(0x398);
  uVar151 = P(0x39c);
  uVar152 = P(0x3a0);
  uVar153 = P(0x3a4);
  uVar154 = P(0x3a8);
  uVar116 = P(0x3ac);
  uVar121 = P(0x3b0);
  uVar155 = P(0x3bc);
  uVar124 = P(0x3b4);
  uVar141 = P(0x3b8);
  uVar156 = P(0x3c0);
  uVar157 = P(0x3c4);
  uVar158 = P(0x3c8);
  uVar159 = P(0x3cc);
  uVar160 = P(0x3d0);
  uVar161 = P(0x3d4);
  uVar162 = P(0x3d8);
  uVar163 = P(0x3dc);
  uVar164 = P(0x3e0);
  uVar165 = P(0x3e4);
  uVar166 = P(0x3e8);
  uVar167 = P(0x3ec);
  uVar168 = P(0x3f0);
  uVar27 = LIN
                     (P(0x340),P(0x344),
                      P(0x348),P(0x34c),
                      P(0x350),P(0x354),
                      P(0x358),P(0x35c),
                      P(0x360),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar28 = LIN
                     (uVar76,uVar66,uVar143,uVar144,uVar145,uVar87,uVar97,uVar102,uVar107,uVar74,
                      uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar29 = LIN
                     (uVar146,uVar147,uVar148,uVar149,uVar150,uVar151,uVar152,uVar153,uVar154,uVar74
                      ,uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar30 = LIN
                     (uVar116,uVar121,uVar124,uVar141,uVar155,uVar156,uVar157,uVar158,uVar159,uVar74
                      ,uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar31 = LIN
                     (uVar160,uVar161,uVar162,uVar163,uVar164,uVar165,uVar166,uVar167,uVar168,uVar74
                      ,uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  iVar11 = NLIN
                     (P(0x3f4),P(0x3f8),
                      P(0x3fc),P(0x400),
                      P(0x404),P(0x408),
                      P(0x40c),P(0x410),
                      P(0x414),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  iVar67 = LIN
                     (P(0x418),P(0x41c),
                      P(0x420),P(0x424),
                      P(0x428),P(0x42c),
                      P(0x430),P(0x434),
                      P(0x438),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar66 = LIN
                     (P(0x43c),P(0x440),
                      P(0x444),P(0x448),
                      P(0x44c),P(0x450),
                      P(0x454),P(0x458),
                      P(0x45c),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  local_308 = P(0x460);
  uVar32 = LIN
                     (local_308,P(0x464),P(0x468),
                      P(0x46c),P(0x470),
                      P(0x474),P(0x478),
                      P(0x47c),P(0x480),uVar74,uVar83,
                      uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  local_2ec = LIN
                        (P(0x484),P(0x488),
                         P(0x48c),P(0x490),
                         P(0x494),P(0x498),
                         P(0x49c),P(0x4a0),
                         P(0x4a4),uVar74,uVar83,uVar140,uVar127,uVar171,
                         uVar138,uVar136,uVar134,uVar131,param_1);
  local_300 = LIN
                        (P(0x4a8),P(0x4ac),
                         P(0x4b0),P(0x4b4),
                         P(0x4b8),P(0x4bc),
                         P(0x4c0),P(0x4c4),
                         P(0x4c8),uVar74,uVar83,uVar140,uVar127,uVar171,
                         uVar138,uVar136,uVar134,uVar131,param_1);
  local_2f8 = LIN
                        (P(0x4cc),P(0x4d0),
                         P(0x4d4),P(0x4d8),
                         P(0x4dc),P(0x4e0),
                         P(0x4e4),P(0x4e8),
                         P(0x4ec),uVar74,uVar83,uVar140,uVar127,uVar171,
                         uVar138,uVar136,uVar134,uVar131,param_1);
  uVar33 = LIN
                     (P(0x4f0),P(0x4f4),
                      P(0x4f8),P(0x4fc),
                      P(0x500),P(0x504),
                      P(0x508),P(0x50c),
                      P(0x510),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  local_25c = LIN
                        (P(0x514),P(0x518),
                         P(0x51c),P(0x520),
                         P(0x524),P(0x528),
                         P(0x52c),P(0x530),
                         P(0x534),uVar74,uVar83,uVar140,uVar127,uVar171,
                         uVar138,uVar136,uVar134,uVar131,param_1);
  uVar34 = LIN
                     (P(0x538),P(0x53c),
                      P(0x540),P(0x544),
                      P(0x548),P(0x54c),
                      P(0x550),P(0x554),
                      P(0x558),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar87 = P(0x580);
  uVar76 = P(0x584);
  uVar141 = P(0x588);
  uVar124 = P(0x58c);
  uVar121 = P(0x590);
  uVar116 = P(0x594);
  uVar107 = P(0x598);
  uVar102 = P(0x59c);
  uVar97 = P(0x5a0);
  uVar35 = LIN
                     (P(0x55c),P(0x560),
                      P(0x564),P(0x568),
                      P(0x56c),P(0x570),
                      P(0x574),P(0x578),
                      P(0x57c),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  iVar68 = LIN
                     (uVar87,uVar76,uVar141,uVar124,uVar121,uVar116,uVar107,uVar102,uVar97,uVar74,
                      uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar117 = P(0x5a8);
  iVar122 = P(0x5ac);
  uVar77 = P(0x5b0);
  uVar88 = P(0x5b4);
  uVar36 = LIN
                     (P(0x5b8),P(0x5bc),
                      P(0x5c0),P(0x5c4),
                      P(0x5c8),P(0x5cc),
                      P(0x5d0),P(0x5d4),
                      P(0x5d8),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar78 = P(0x5dc);
  uVar89 = P(0x5e0);
  uVar98 = P(0x5e4);
  uVar103 = P(0x5e8);
  uVar108 = P(0x5ec);
  uVar118 = P(0x5f0);
  uVar123 = P(0x5f4);
  uVar79 = P(0x5f8);
  uVar90 = P(0x5fc);
  uVar37 = LIN
                     (P(0x600),P(0x604),
                      P(0x608),P(0x60c),
                      P(0x610),P(0x614),
                      P(0x618),P(0x61c),
                      P(0x620),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar38 = LIN
                     (P(0x624),P(0x628),
                      P(0x62c),P(0x630),
                      P(0x634),P(0x638),
                      P(0x63c),P(0x640),
                      P(0x644),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar39 = LIN
                     (P(0x648),P(0x64c),
                      P(0x650),P(0x654),
                      P(0x658),P(0x65c),
                      P(0x660),P(0x664),
                      P(0x668),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar104 = P(0x670);
  iVar99 = P(0x66c);
  uVar40 = LIN
                     (P(0x674),P(0x678),
                      P(0x67c),P(0x680),
                      P(0x684),P(0x688),
                      P(0x68c),P(0x690),
                      P(0x694),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar41 = LIN
                     (P(0x698),P(0x69c),
                      P(0x6a0),P(0x6a4),
                      P(0x6a8),P(0x6ac),
                      P(0x6b0),P(0x6b4),
                      P(0x6b8),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  local_318 = P(0x6bc);
  uVar109 = P(0x6c0);
  uVar42 = LIN
                     (P(0x6c4),P(0x6c8),
                      P(0x6cc),P(0x6d0),
                      P(0x6d4),P(0x6d8),
                      P(0x6dc),P(0x6e0),
                      P(0x6e4),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar43 = LIN
                     (P(0x6e8),P(0x6ec),
                      P(0x6f0),P(0x6f4),
                      P(0x6f8),P(0x6fc),
                      P(0x700),P(0x704),
                      P(0x708),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar87 = P(0x738);
  uVar76 = P(0x73c);
  uVar141 = P(0x740);
  uVar124 = P(0x744);
  uVar121 = P(0x748);
  local_31c = P(0x70c);
  uVar116 = P(0x74c);
  uVar107 = P(0x750);
  uVar102 = P(0x754);
  uVar97 = P(0x758);
  uVar173 = P(0x710);
  uVar44 = LIN
                     (P(0x714),P(0x718),
                      P(0x71c),P(0x720),
                      P(0x724),P(0x728),
                      P(0x72c),P(0x730),
                      P(0x734),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar45 = LIN
                     (uVar87,uVar76,uVar141,uVar124,uVar121,uVar116,uVar107,uVar102,uVar97,uVar74,
                      uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar124 = P(0x790);
  uVar87 = P(0x788);
  uVar76 = P(0x78c);
  uVar141 = P(0x794);
  iVar142 = P(0x75c);
  uVar121 = P(0x798);
  uVar174 = P(0x760);
  uVar116 = P(0x79c);
  uVar107 = P(0x7a0);
  uVar102 = P(0x7a4);
  uVar97 = P(0x7a8);
  uVar46 = LIN
                     (P(0x764),P(0x768),
                      P(0x76c),P(0x770),
                      P(0x774),P(0x778),
                      P(0x77c),P(0x780),
                      P(0x784),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar47 = LIN
                     (uVar87,uVar76,uVar124,uVar141,uVar121,uVar116,uVar107,uVar102,uVar97,uVar74,
                      uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar124 = P(0x7d8);
  uVar87 = P(0x7d0);
  uVar76 = P(0x7d4);
  uVar141 = P(0x7dc);
  uVar121 = P(0x7e0);
  uVar116 = P(0x7e4);
  uVar107 = P(0x7e8);
  uVar102 = P(0x7ec);
  uVar97 = P(0x7f0);
  uVar48 = LIN
                     (P(0x7ac),P(0x7b0),
                      P(0x7b4),P(0x7b8),
                      P(0x7bc),P(0x7c0),
                      P(0x7c4),P(0x7c8),
                      P(0x7cc),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar49 = LIN
                     (uVar87,uVar76,uVar124,uVar141,uVar121,uVar116,uVar107,uVar102,uVar97,uVar74,
                      uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar50 = NLIN
                     (P(0x7f4),P(0x7f8),
                      P(0x7fc),P(0x800),
                      P(0x804),P(0x808),
                      P(0x80c),P(0x810),
                      P(0x814),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar51 = LIN
                     (P(0x818),P(0x81c),
                      P(0x820),P(0x824),
                      P(0x828),P(0x82c),
                      P(0x830),P(0x834),
                      P(0x838),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar52 = LIN
                     (P(0x83c),P(0x840),
                      P(0x844),P(0x848),
                      P(0x84c),P(0x850),
                      P(0x854),P(0x858),
                      P(0x85c),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar87 = P(0x884);
  uVar141 = P(0x88c);
  uVar76 = P(0x888);
  uVar143 = P(0x890);
  uVar144 = P(0x894);
  uVar97 = P(0x898);
  uVar145 = P(0x89c);
  uVar102 = P(0x8a0);
  uVar107 = P(0x8a4);
  uVar146 = P(0x8a8);
  uVar147 = P(0x8ac);
  uVar148 = P(0x8b0);
  uVar149 = P(0x8b4);
  uVar150 = P(0x8b8);
  uVar151 = P(0x8bc);
  uVar152 = P(0x8c0);
  uVar153 = P(0x8c4);
  uVar154 = P(0x8c8);
  uVar116 = P(0x8cc);
  uVar121 = P(0x8d0);
  uVar155 = P(0x8d8);
  uVar124 = P(0x8d4);
  uVar156 = P(0x8dc);
  uVar157 = P(0x8e0);
  uVar158 = P(0x8e4);
  uVar159 = P(0x8e8);
  uVar160 = P(0x8ec);
  uVar161 = P(0x8f0);
  uVar162 = P(0x8f4);
  uVar163 = P(0x8f8);
  uVar164 = P(0x8fc);
  uVar165 = P(0x900);
  uVar166 = P(0x904);
  uVar167 = P(0x908);
  uVar168 = P(0x90c);
  uVar169 = P(0x910);
  uVar53 = LIN
                     (P(0x860),P(0x864),
                      P(0x868),P(0x86c),
                      P(0x870),P(0x874),
                      P(0x878),P(0x87c),
                      P(0x880),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar54 = LIN
                     (uVar87,uVar76,uVar141,uVar143,uVar144,uVar97,uVar145,uVar102,uVar107,uVar74,
                      uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar55 = LIN
                     (uVar146,uVar147,uVar148,uVar149,uVar150,uVar151,uVar152,uVar153,uVar154,uVar74
                      ,uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar56 = LIN
                     (uVar116,uVar121,uVar124,uVar155,uVar156,uVar157,uVar158,uVar159,uVar160,uVar74
                      ,uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar57 = LIN
                     (uVar161,uVar162,uVar163,uVar164,uVar165,uVar166,uVar167,uVar168,uVar169,uVar74
                      ,uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar58 = NLIN
                     (P(0x914),P(0x918),
                      P(0x91c),P(0x920),
                      P(0x924),P(0x928),
                      P(0x92c),P(0x930),
                      P(0x934),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  iVar69 = LIN
                     (P(0x938),P(0x93c),
                      P(0x940),P(0x944),
                      P(0x948),P(0x94c),
                      P(0x950),P(0x954),
                      P(0x958),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  local_170 = LIN
                        (P(0x95c),P(0x960),
                         P(0x964),P(0x968),
                         P(0x96c),P(0x970),
                         P(0x974),P(0x978),
                         P(0x97c),uVar74,uVar83,uVar140,uVar127,uVar171,
                         uVar138,uVar136,uVar134,uVar131,param_1);
  local_320 = P(0x980);
  uVar59 = LIN
                     (local_320,P(0x984),P(0x988),
                      P(0x98c),P(0x990),
                      P(0x994),P(0x998),
                      P(0x99c),P(0x9a0),uVar74,uVar83,
                      uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar124 = P(0x9a4);
  uVar141 = P(0x9a8);
  uVar97 = P(0x9ac);
  uVar102 = P(0x9b0);
  uVar143 = P(0x9b4);
  uVar144 = P(0x9b8);
  uVar145 = P(0x9bc);
  uVar146 = P(0x9c0);
  uVar147 = P(0x9c4);
  uVar107 = P(0x9c8);
  uVar116 = P(0x9cc);
  uVar148 = P(0x9d4);
  uVar121 = P(0x9d0);
  uVar149 = P(0x9d8);
  uVar150 = P(0x9dc);
  uVar151 = P(0x9e0);
  uVar152 = P(0x9e4);
  uVar153 = P(0x9e8);
  uVar87 = P(0x9ec);
  uVar76 = P(0x9f0);
  uVar154 = P(0x9f4);
  uVar155 = P(0x9f8);
  uVar156 = P(0x9fc);
  uVar157 = P(0xa00);
  uVar158 = P(0xa04);
  uVar159 = P(0xa08);
  uVar160 = P(0xa0c);
  uVar161 = P(0xa10);
  uVar162 = P(0xa14);
  uVar163 = P(0xa18);
  uVar164 = P(0xa1c);
  uVar165 = P(0xa20);
  uVar166 = P(0xa24);
  uVar167 = P(0xa28);
  uVar168 = P(0xa2c);
  uVar169 = P(0xa30);
  local_304 = LIN
                        (P(0xa34),P(0xa38),
                         P(0xa3c),P(0xa40),
                         P(0xa44),P(0xa48),
                         P(0xa4c),P(0xa50),
                         P(0xa54),uVar74,uVar83,uVar140,uVar127,uVar171,
                         uVar138,uVar136,uVar134,uVar131,param_1);
  uVar60 = LIN
                     (uVar87,uVar76,uVar154,uVar155,uVar156,uVar157,uVar158,uVar159,uVar160,uVar74,
                      uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  local_310 = LIN
                        (uVar124,uVar141,uVar97,uVar102,uVar143,uVar144,uVar145,uVar146,uVar147,
                         uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,
                         param_1);
  local_330 = LIN
                        (uVar107,uVar116,uVar121,uVar148,uVar149,uVar150,uVar151,uVar152,uVar153,
                         uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,
                         param_1);
  uVar61 = LIN
                     (uVar161,uVar162,uVar163,uVar164,uVar165,uVar166,uVar167,uVar168,uVar169,uVar74
                      ,uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar124 = P(0xa7c);
  uVar141 = P(0xa80);
  uVar116 = P(0xa84);
  uVar87 = P(0xa8c);
  uVar76 = P(0xa90);
  uVar107 = P(0xa94);
  uVar102 = P(0xa98);
  uVar97 = P(0xa9c);
  uVar121 = P(0xa88);
  uVar62 = LIN
                     (P(0xa58),P(0xa5c),
                      P(0xa60),P(0xa64),
                      P(0xa68),P(0xa6c),
                      P(0xa70),P(0xa74),
                      P(0xa78),uVar74,uVar83,uVar140,uVar127,uVar171,uVar138,
                      uVar136,uVar134,uVar131,param_1);
  uVar63 = LIN
                     (uVar124,uVar141,uVar116,uVar121,uVar87,uVar76,uVar107,uVar102,uVar97,uVar74,
                      uVar83,uVar140,uVar127,uVar171,uVar138,uVar136,uVar134,uVar131,param_1);
  uVar126 = iVar67 / 9;
  uVar64 = (uVar126 * 9 - iVar67) + 8;
  if (local_2ec < 5) {
    local_2ec = 5;
    if (iVar100 == 0) goto LAB_000ecea8;
LAB_000ecf70:
    local_e0 = 0x12c19005;
    uVar132 = 300;
    uVar125 = 100;
    local_2f8 = 5;
    local_300 = local_308;
    local_2f4 = local_308;
    local_25c = local_308;
LAB_000ecf9c:
    if (4 < local_2e4) goto LAB_000ecfac;
LAB_000ecf38:
    local_2e4 = 5;
    if (99 < (int)local_2e8) goto LAB_000ecfc4;
LAB_000ecf48:
    local_dc = 100;
    iVar67 = 0x50;
    local_2e8 = 100;
  }
  else {
    if (0x5f < local_2ec) {
      local_2ec = 0x5f;
    }
    if (iVar100 != 0) goto LAB_000ecf70;
LAB_000ecea8:
    local_308 = local_300;
    if (9 < (int)uVar34) {
      uVar125 = local_2f8 + 5;
      uVar132 = local_2f8 + 10;
      local_e0 = (uVar132 & 0x1ff) << 0x14 | (uVar125 & 0x1ff) << 10 | local_2f8 & 0x1ff;
      local_300 = (int)((local_25c - local_300) * local_2ec) / 100 + local_300;
      local_2f4 = local_25c;
      local_25c = uVar34;
      goto LAB_000ecf9c;
    }
    uVar125 = local_2f8 + 5;
    uVar132 = local_2f8 + 10;
    local_e0 = (uVar132 & 0x1ff) << 0x14 | (uVar125 & 0x1ff) << 10 | local_2f8 & 0x1ff;
    local_2f4 = (int)((local_25c - local_300) * local_2ec) / 100 + local_300;
    if (local_2e4 < 5) goto LAB_000ecf38;
LAB_000ecfac:
    if (0x5f < local_2e4) {
      local_2e4 = 0x5f;
    }
    if ((int)local_2e8 < 100) goto LAB_000ecf48;
LAB_000ecfc4:
    local_dc = local_2e8 & 0xff;
    iVar67 = local_2e8 - 0x14;
  }
  iVar67 = (local_2e4 * iVar67) / 100;
  uVar34 = local_2e8 - iVar67;
  iVar70 = (int)((uVar14 - uVar112) * local_2e4) / 100;
  uVar80 = uVar14 - iVar70;
  if (local_2dc < 5) {
    local_2dc = 5;
  }
  else if (0x5f < local_2dc) {
    local_2dc = 0x5f;
  }
  iVar1 = (int)((200 - uVar16) * local_2dc) / 100;
  uVar71 = iVar1 + uVar16;
  iVar2 = (int)((uVar17 - uVar114) * local_2dc) / 100;
  uVar81 = uVar17 - iVar2;
  if (local_2e0 < 5) {
    local_2e0 = 5;
  }
  else if (0x5f < local_2e0) {
    local_2e0 = 0x5f;
  }
  if ((int)local_264 < 100) {
    local_d8 = 100;
    iVar82 = 0x50;
    local_264 = 100;
  }
  else {
    local_d8 = local_264 & 0xff;
    iVar82 = local_264 - 0x14;
  }
  iVar82 = (local_2e0 * iVar82) / 100;
  uVar72 = local_264 - iVar82;
  iVar3 = (int)((uVar15 - uVar113) * local_2e0) / 100;
  uVar128 = uVar15 - iVar3;
  if (local_2d8 < 5) {
    local_2d8 = 5;
  }
  else if (0x5f < local_2d8) {
    local_2d8 = 0x5f;
  }
  iVar4 = (int)((200 - uVar18) * local_2d8) / 100;
  uVar73 = iVar4 + uVar18;
  iVar5 = (int)((uVar19 - uVar115) * local_2d8) / 100;
  uVar129 = uVar19 - iVar5;
  if (iVar100 == 1) {
    local_d4 = 0;
  }
  else {
    local_d4 = (uVar23 & 1) << 8;
  }
  local_238 = iVar94 - 0x32;
  if (0x31 >= iVar94) {
    local_238 = 0x32 - iVar94;
  }
  bVar6 = 0x31 >= iVar94;
  if (local_234 < 0x32) {
    local_234 = 0x32 - local_234;
    iVar94 = 0x10 - uVar35;
    local_6c = 1;
    if (0xf < iVar94) goto LAB_000eecb4;
LAB_000ed248:
    local_d0 = (uVar35 & 0x1f) << 8 | 0x10;
    iVar100 = iVar94 << 1;
    local_284 = (uint)(((double)iVar94 * 8.0
                       ) / 10.0);
    local_cc = local_284 & 0x3f;
    local_198 = uVar35;
    if (0xf < iVar100) goto LAB_000eece4;
LAB_000ed2a0:
    iVar100 = iVar100 + iVar94;
    local_260 = (uVar35 - 8) * 2;
    local_21c = (local_260 & 0x1f) << 0x10;
    if (0xf < iVar100) goto LAB_000eecf8;
LAB_000ed2c8:
    local_29c = 0x10 - iVar100;
    local_c8 = (local_29c & 0x1f) << 0x18;
    local_280 = (uint)(((double)(int)local_29c *
                       8.0) /
                      30.0);
    local_c4 = (local_280 & 0x3f) << 0x18;
  }
  else {
    local_234 = local_234 + -0x32;
    iVar94 = 0x10 - uVar35;
    local_6c = 0;
    if (iVar94 < 0x10) goto LAB_000ed248;
LAB_000eecb4:
    iVar100 = iVar94 << 1;
    local_cc = 0xc;
    local_d0 = 0x10;
    local_284 = 0xc;
    local_198 = 0;
    if (iVar100 < 0x10) goto LAB_000ed2a0;
LAB_000eece4:
    iVar100 = iVar100 + iVar94;
    local_21c = 0;
    local_260 = 0;
    if (iVar100 < 0x10) goto LAB_000ed2c8;
LAB_000eecf8:
    local_c4 = 0;
    local_c8 = 0;
    local_280 = 0;
    local_29c = 0;
  }
  if (iVar68 == 3) {
LAB_000ed340:
    local_74 = (uint32_t)0x0;
    local_58 = 0;
    local_5c = 0;
    local_60 = 0;
    local_64 = 0;
    local_68 = 0;
  }
  else if (iVar68 == 2) {
    local_74 = (uint32_t)0x21100;
    local_58 = 2;
    local_5c = 1;
    local_60 = 1;
    local_64 = 0;
    local_68 = 0;
  }
  else if (iVar68 == 1) {
    local_74 = (uint32_t)0x32210;
    local_58 = 3;
    local_5c = 2;
    local_60 = 2;
    local_64 = 1;
    local_68 = 0;
  }
  else {
    if (iVar68 != 0) goto LAB_000ed340;
    local_58 = 3;
    local_74 = (uint32_t)0x33221;
    local_5c = 3;
    local_60 = 2;
    local_64 = 2;
    local_68 = 1;
  }
  uVar23 = 8 - iVar69;
  if (local_310 < 5) {
    local_310 = 5;
  }
  else if (0x5f < local_310) {
    local_310 = 0x5f;
  }
  if (iVar122 == 0) {
    local_220 = uVar60;
    if ((int)local_304 < 10) {
      uVar35 = uVar60 + 5;
      local_19c = (uVar35 & 0x1ff) << 0x10 | uVar60 & 0x1ff;
      local_320 = (int)((uVar61 - local_330) * local_310) / 100 + local_330;
      local_304 = uVar61;
    }
    else {
      uVar35 = uVar60 + 5;
      local_19c = (uVar35 & 0x1ff) << 0x10 | uVar60 & 0x1ff;
      local_330 = (int)((uVar61 - local_330) * local_310) / 100 + local_330;
      local_320 = uVar61;
    }
  }
  else {
    local_19c = 0x12c0005;
    uVar35 = 300;
    local_220 = 5;
    local_330 = local_320;
    local_304 = local_320;
  }
  local_250 = uVar60 & 0x1ff;
  if (iVar99 < 5) {
    iVar99 = 5;
  }
  else if (0x5f < iVar99) {
    iVar99 = 0x5f;
  }
  uVar61 = 100;
  if ((int)uVar40 < 0x65) {
    uVar61 = uVar40;
  }
  iVar68 = (int)((uVar61 - 0x14) * iVar99) / 100;
  uVar40 = uVar61 - iVar68;
  if (local_31c < 5) {
    local_31c = 5;
  }
  else if (0x5f < local_31c) {
    local_31c = 0x5f;
  }
  iVar69 = (int)((200 - uVar44) * local_31c) / 100;
  uVar91 = iVar69 + uVar44;
  if ((int)local_318 < 5) {
    local_318 = 5;
  }
  else if (0x5f < (int)local_318) {
    local_318 = 0x5f;
  }
  uVar172 = 100;
  if ((int)uVar42 < 0x65) {
    uVar172 = uVar42;
  }
  iVar94 = (int)((uVar172 - 0x14) * local_318) / 100;
  uVar42 = uVar172 - iVar94;
  if (iVar142 < 5) {
    iVar142 = 5;
  }
  else if (0x5f < iVar142) {
    iVar142 = 0x5f;
  }
  uVar50 = uVar50 & 1;
  if (iVar122 == 1) {
    uVar50 = 0;
  }
  iVar99 = (int)((200 - uVar46) * iVar142) / 100;
  uVar92 = iVar99 + uVar46;
  if (uVar65 == 0x22a) {
    local_170 = (uint)((double)(int)local_170 * 0.5);
  }
  if (uVar125 == local_2f8) {
    local_208 = 0;
    local_2f0 = 0;
    if (uVar132 != uVar125) goto LAB_000ed58c;
LAB_000f24b4:
    local_1a0 = 0;
    local_2bc = 0;
    if (uVar132 != 500) goto LAB_000ed5dc;
LAB_000f24c8:
    dVar176 = 8.0;
    local_c0 = 0;
    local_2b8 = 0;
    local_318 = (uint)(((double)(int)local_308 * dVar176) /
                      10.0);
    if (local_2e8 != uVar34) goto LAB_000ed648;
LAB_000f24f8:
    local_78 = 0;
    local_288 = 0;
LAB_000ed688:
    uVar65 = (uint)(((double)(int)(uVar80 - uVar112) *
                    8.0) /
                   (double)(int)(uVar34 - 0x14));
    local_bc = (uVar65 & 0x3ff) << 0x10;
  }
  else {
    local_2f0 = (uint)(((double)(int)(local_25c - local_2f4) *
                       8.0) /
                      (double)(int)(uVar125 - local_2f8));
    local_208 = local_2f0 & 0x3ff;
    if (uVar132 == uVar125) goto LAB_000f24b4;
LAB_000ed58c:
    local_2bc = (uint)(((double)(int)(local_2f4 - local_300) *
                       8.0) /
                      (double)(int)(uVar132 - uVar125));
    local_1a0 = (local_2bc & 0x3ff) << 0x10;
    if (uVar132 == 500) goto LAB_000f24c8;
LAB_000ed5dc:
    local_2b8 = (uint)(((double)(int)(local_300 - local_308) *
                       8.0) /
                      (double)(int)(500 - uVar132));
    dVar176 = 8.0;
    local_c0 = local_2b8 & 0x3ff;
    local_318 = (uint)(((double)(int)local_308 * dVar176) /
                      10.0);
    if (local_2e8 == uVar34) goto LAB_000f24f8;
LAB_000ed648:
    local_288 = (uint)(((double)iVar70 * dVar176) / (double)iVar67);
    local_78 = local_288 & 0x3ff;
    if (uVar34 != 0x14) goto LAB_000ed688;
    local_bc = 0;
    uVar65 = 0;
  }
  uVar177 = (uint)(((double)(int)uVar112 *
                   8.0) /
                  10.0);
  if (uVar16 == uVar71) {
    local_b8 = 0;
    local_2b4 = 0;
  }
  else {
    local_2b4 = (uint)(((double)iVar2 * 8.0)
                      / (double)iVar1);
    local_b8 = local_2b4 & 0x3ff;
  }
  if (uVar71 == 200) {
    local_b4 = 0;
    local_2b0 = 0;
  }
  else {
    local_2b0 = (uint)(((double)(int)(uVar81 - uVar114) *
                       8.0) /
                      (double)(int)(200 - uVar71));
    local_b4 = (local_2b0 & 0x3ff) << 0x10;
  }
  uVar179 = (uint)(((double)(int)uVar114 *
                   8.0) /
                  20.0);
  if (local_264 == uVar72) {
    local_7c = 0;
    uVar182 = 0;
  }
  else {
    uVar182 = (uint)(((double)iVar3 * 8.0) /
                    (double)iVar82);
    local_7c = uVar182 & 0x3ff;
    if (uVar72 == 0x14) {
      local_b0 = 0;
      uVar180 = 0;
      goto LAB_000ed82c;
    }
  }
  uVar180 = (uint)(((double)(int)(uVar128 - uVar113) *
                   8.0) /
                  (double)(int)(uVar72 - 0x14));
  local_b0 = (uVar180 & 0x3ff) << 0x10;
LAB_000ed82c:
  uVar178 = (uint)(((double)(int)uVar113 *
                   8.0) /
                  10.0);
  if (uVar18 == uVar73) {
    local_ac = 0;
    local_2ac = 0;
  }
  else {
    local_2ac = (uint)(((double)iVar5 * 8.0)
                      / (double)iVar4);
    local_ac = local_2ac & 0x3ff;
  }
  if (uVar73 == 200) {
    local_a8 = 0;
    local_2a8 = 0;
  }
  else {
    local_2a8 = (uint)(((double)(int)(uVar129 - uVar115) *
                       8.0) /
                      (double)(int)(200 - uVar73));
    local_a8 = (local_2a8 & 0x3ff) << 0x10;
  }
  dVar176 = 8.0;
  uVar175 = (uint)(((double)(int)uVar115 * dVar176) /
                  20.0);
  uVar181 = (uint)(((double)(int)(local_198 - local_260) * dVar176) /
                  10.0);
  uVar183 = (uint)(((double)(int)(local_260 - local_29c) * dVar176) /
                  20.0);
  if (uVar35 == local_220) {
    local_a4 = 0;
    local_2a4 = 0;
  }
  else {
    local_2a4 = (uint)(((double)(int)(local_304 - local_320) *
                       4.0) /
                      (double)(int)(uVar35 - local_220));
    local_a4 = local_2a4 & 0x1ff;
  }
  if (uVar35 == 500) {
    local_a0 = 0;
    local_2a0 = 0;
  }
  else {
    local_2a0 = (uint)(((double)(int)(local_320 - local_330) *
                       4.0) /
                      (double)(int)(500 - uVar35));
    local_a0 = (local_2a0 & 0x1ff) << 0x10;
  }
  uVar184 = (uint)(((double)(int)local_330 *
                   4.0) /
                  10.0);
  if (uVar40 == uVar61) {
    local_9c = 0;
    local_298 = 0;
  }
  else {
    local_298 = (uint)(((double)(int)(uVar41 - uVar104) *
                       4.0) / (double)iVar68
                      );
    local_9c = local_298 & 0x1ff;
  }
  if (uVar40 == 0x14) {
    local_98 = 0;
    local_294 = 0;
  }
  else {
    local_294 = (uint)(((double)(int)uVar104 *
                       4.0) /
                      (double)(int)(uVar40 - 0x14));
    local_98 = (local_294 & 0x1ff) << 0x10;
  }
  if (uVar44 == uVar91) {
    local_94 = 0;
    local_2d4 = 0;
  }
  else {
    local_2d4 = (uint)(((double)(int)(uVar45 - uVar173) *
                       4.0) / (double)iVar69
                      );
    local_94 = local_2d4 & 0x1ff;
  }
  if (uVar91 == 200) {
    local_90 = 0;
    local_2d0 = 0;
  }
  else {
    local_2d0 = (uint)(((double)(int)uVar173 *
                       4.0) /
                      (double)(int)(200 - uVar91));
    local_90 = (local_2d0 & 0x1ff) << 0x10;
  }
  if (uVar42 == uVar172) {
    local_8c = 0;
    local_290 = 0;
  }
  else {
    local_290 = (uint)(((double)(int)(uVar43 - uVar109) *
                       4.0) / (double)iVar94
                      );
    local_8c = local_290 & 0x1ff;
  }
  if (uVar42 == 0x14) {
    local_88 = 0;
    local_28c = 0;
  }
  else {
    local_28c = (uint)(((double)(int)uVar109 *
                       4.0) /
                      (double)(int)(uVar42 - 0x14));
    local_88 = (local_28c & 0x1ff) << 0x10;
  }
  if (uVar46 == uVar92) {
    local_84 = 0;
    local_2cc = 0;
  }
  else {
    local_2cc = (uint)(((double)(int)(uVar47 - uVar174) *
                       4.0) / (double)iVar99
                      );
    local_84 = local_2cc & 0x1ff;
  }
  if (uVar92 == 200) {
    local_80 = 0;
    local_2c8 = 0;
  }
  else {
    local_2c8 = (uint)(((double)(int)uVar174 *
                       4.0) /
                      (double)(int)(200 - uVar92));
    local_80 = (local_2c8 & 0x1ff) << 0x10;
  }
  if (0x3ff < (int)local_2f0) {
  }
  if (0x3ff < (int)local_2bc) {
  }
  if (0x3ff < (int)local_2b8) {
  }
  if (0x3ff < (int)local_318) {
  }
  if (0x3ff < (int)local_288) {
  }
  if (0x3ff < (int)uVar65) {
  }
  if (0x3ff < (int)uVar177) {
  }
  if (0x3ff < (int)local_2b4) {
  }
  if (0x3ff < (int)local_2b0) {
  }
  if (0x3ff < (int)uVar179) {
  }
  if (0x3ff < (int)uVar182) {
  }
  if (0x3ff < (int)uVar180) {
  }
  if (0x3ff < (int)uVar178) {
  }
  if (0x3ff < (int)local_2ac) {
  }
  if (0x3ff < (int)local_2a8) {
  }
  if (0x3ff < (int)uVar175) {
  }
  if (0x3f < (int)local_284) {
  }
  if (0x3f < (int)uVar181) {
  }
  if (0x3f < (int)uVar183) {
  }
  if (0x3f < (int)local_280) {
  }
  if (0x1ff < (int)local_2a4) {
  }
  if (0x1ff < (int)local_2a0) {
  }
  if (0x1ff < (int)uVar184) {
  }
  if (0x1ff < (int)local_298) {
  }
  if (0x1ff < (int)local_294) {
  }
  if (0x1ff < (int)local_2d4) {
  }
  if (0x1ff < (int)local_2d0) {
  }
  if (0x1ff < (int)local_290) {
  }
  if (0x1ff < (int)local_28c) {
  }
  if (0x1ff < (int)local_2cc) {
  }
  if (0x1ff < (int)local_2c8) {
  }
  if (uVar10 == 0) {
  }
  else {
  }
  W(0x180,3);
  W(0x184,0xf0966414);
  W(0x188,0x101010);
  W(0x18c,0x1000000);
  W(400,0xfa966414);
  W(0x194,0x80808);
  W(0x198,0);
  W(0x19c,uVar12 & 0xff | 0xaf50000 | (uVar13 & 0xff) << 8);
  W(0x1a0,uVar133 & 3 | (uVar96 & 1) << 4);
  W(0x1a4,0x3b403b4);
  W(0x1a8,0xc832);
  W(0x1ac,0xc832);
  W(0x1b0,0x10a0a);
  uVar65 = (uVar64 & 0xf) << 4;
  W(0x1d0,(uVar25 & 1) << 10 | uVar126 & 3 | (uVar93 & 1) << 9 | uVar65);
  W(0x1d4,local_e0);
  W(0x1d8,0x7f9f4);
  W(0x1dc,(local_2f4 & 0x1ff) << 0x10 | local_25c & 0x1ff);
  W(0x1e0,(local_308 & 0x1ff) << 0x10 | local_300 & 0x1ff);
  W(0x1e4,local_1a0 | local_208);
  W(0x1e8,local_c0 | (local_318 & 0x3ff) << 0x10);
  W(0x1ec,(uVar30 & 0x3f) << 0x18 | (uVar29 & 0x3f) << 0x10 | uVar27 & 0x3f |
                            (uVar28 & 0x3f) << 8);
  W(0x1f0,uVar31 & 0x3f);
  W(500,0);
  W(0x1f8,0);
  W(0x1fc,0xffff);
  W(0x200,0x1416);
  W(0x204,100);
  W(0x208,(uVar110 & 0x3f) << 0x18 | (uVar110 & 0x3f) << 0x10 | uVar105 & 0x3ff);
  W(0x220,(uVar20 & 3) << 0x1d | (uVar130 & 0x1ff) << 0x14 | uVar21 & 0x1ff |
                            (uVar22 & 0x1ff) << 10);
  W(0x224,(uVar34 & 0xff) << 0x10 | local_dc);
  W(0x228,0xa0014);
  W(0x22c,(uVar80 & 0x1ff) << 0x10 | uVar14 & 0x1ff);
  W(0x230,uVar112 & 0x1ff);
  W(0x234,local_78 | local_bc);
  W(0x238,uVar177 & 0x3ff);
  W(0x23c,(uVar71 & 0xff) << 0x10 | uVar16 & 0xff);
  W(0x240,0xdc00c8);
  W(0x244,(uVar81 & 0x1ff) << 0x10 | uVar17 & 0x1ff);
  W(0x248,uVar114 & 0x1ff);
  W(0x24c,local_b4 | local_b8);
  W(0x250,uVar179 & 0x3ff);
  W(0x254,(uVar106 & 7) << 0x18 | (uVar101 & 0xff) << 0x10 | uVar85 & 0xff |
                            (uVar95 & 7) << 8);
  W(600,(uVar72 & 0xff) << 0x10 | local_d8);
  W(0x25c,0xa0014);
  W(0x260,(uVar128 & 0x1ff) << 0x10 | uVar15 & 0x1ff);
  W(0x264,uVar113 & 0x1ff);
  W(0x268,local_7c | local_b0);
  W(0x26c,uVar178 & 0x3ff);
  W(0x270,(uVar73 & 0xff) << 0x10 | uVar18 & 0xff);
  W(0x274,0xdc00c8);
  W(0x278,(uVar129 & 0x1ff) << 0x10 | uVar19 & 0x1ff);
  W(0x27c,uVar115 & 0x1ff);
  W(0x280,local_a8 | local_ac);
  W(0x284,uVar175 & 0x3ff);
  W(0x288,(uVar86 & 7) << 0x18 | (uVar75 & 0xff) << 0x10 | uVar111 & 0xff |
                            (uVar120 & 7) << 8);
  W(0x2a0,(uVar32 & 0x1ff) << 0x14 | (uVar24 & 0xff) << 0xc | uVar26 & 0x1f |
                            local_d4);
  W(0x2a4,uVar32 & 0x1ff | 0x320000);
  iVar67 = ncu_img_h;
  iVar68 = ncu_img_w;
  uVar10 = iVar67 / iVar11;
  if (iVar11 == 0) {
  }
  iVar69 = iVar11;
  if (iVar11 < 0) {
    iVar69 = iVar11 + 0xf;
  }
  uVar130 = iVar68 / iVar11;
  if (iVar11 == 0) {
  }
  if (iVar67 % iVar11 != 0) {
    uVar10 = uVar10 + 1;
  }
  if (iVar68 % iVar11 != 0) {
    uVar130 = uVar130 + 1;
  }
  W(0x2a8,uVar65 | uVar126 & 3);
  uVar65 = iVar69 >> 4 & 0xff;
  W(0x2ac,uVar65 << 0x10 | uVar65);
  W(0x2c4,uVar10 & 0x3ff | (uVar130 & 0xff) << 0x10);
  W(0x2b0,0);
  W(0x2b4,(uVar33 & 0x1ff) << 0x10 | uVar33 & 0x1ff);
  W(0x2b8,uVar66);
  uVar10 = iVar11 % 0x10 & 0xf;
  W(0x2c0,uVar10 << 8 | uVar10);
  if (local_234 == 0) {
    local_6c = 0;
  }
  if (local_6c != 0) {
    local_234 = 0x80 - local_234;
  }
  if (local_238 != 0 && bVar6) {
    local_238 = 0x80 - local_238;
  }
  W(700,(uVar119 & 0xff) << 0x10 | uVar84 & 0xff | local_234 << 0x18 |
                          (local_238 & 0xff) << 8);
  W(0x2d0,0x4628140a);
  W(0x2d4,local_21c | local_d0 | local_c8);
  W(0x2d8,(uVar181 & 0x3f) << 8 | local_cc | local_c4 | (uVar183 & 0x3f) << 0x10);
  W(0x2dc,local_74);
  W(0x300,0x10a0a);
  W(0x304,0xf08214);
  W(0x308,0x100010);
  W(0x30c,0);
  W(0x310,0xf08214);
  W(0x314,0x80008);
  W(0x318,0);
  W(0x31c,(uVar37 & 3) << 0x10 | (uVar39 & 0xff) << 8 | uVar38 & 0xff);
  W(800,(uVar90 & 1) << 0x10 | 0xaf5);
  W(0x324,0x3b403b4);
  W(0x328,0xc832);
  W(0x32c,0xc832);
  W(0x340,(uVar23 & 0xf) << 4 | uVar117 & 1 | 0x640000);
  W(0x344,0xff00ff);
  W(0x348,local_19c);
  W(0x34c,0x1fe01f4);
  W(0x350,(local_320 & 0x1ff) << 0x10 | local_304 & 0x1ff);
  W(0x354,local_330 & 0x1ff);
  W(0x358,local_a0 | local_a4);
  W(0x35c,uVar184 & 0x1ff);
  W(0x360,(uVar56 & 0x3f) << 0x18 | (uVar55 & 0x3f) << 0x10 | uVar53 & 0x3f |
                            (uVar54 & 0x3f) << 8);
  W(0x364,uVar57 & 0x3f);
  W(0x368,0);
  W(0x36c,0);
  W(0x370,(uVar88 & 0x3f) << 0x18 | (uVar88 & 0x3f) << 0x10 | uVar77 & 0x3ff);
  W(0x390,uVar36 & 0x1ff);
  W(0x394,uVar61 & 0xff | 0x140000 | (uVar40 & 0xff) << 8);
  W(0x398,uVar41 & 0x1ff | (uVar104 & 0x1ff) << 0x10);
  W(0x39c,local_98 | local_9c);
  W(0x3a0,uVar44 & 0xff | 0xc80000 | (uVar91 & 0xff) << 8);
  W(0x3a4,uVar45 & 0x1ff | (uVar173 & 0x1ff) << 0x10);
  W(0x3a8,local_90 | local_94);
  W(0x3ac,uVar48 & 0x1ff);
  W(0x3b0,(uVar103 & 7) << 0x18 | (uVar98 & 0xff) << 0x10 | uVar78 & 0xff |
                            (uVar89 & 7) << 8);
  W(0x3b4,uVar172 & 0xff | 0x140000 | (uVar42 & 0xff) << 8);
  W(0x3b8,uVar43 & 0x1ff | (uVar109 & 0x1ff) << 0x10);
  W(0x3bc,local_88 | local_8c);
  W(0x3c0,uVar46 & 0xff | 0xc80000 | (uVar92 & 0xff) << 8);
  W(0x3c4,uVar47 & 0x1ff | (uVar174 & 0x1ff) << 0x10);
  W(0x3c8,local_80 | local_84);
  W(0x3cc,uVar49 & 0x1ff);
  W(0x3d0,(uVar79 & 7) << 0x18 | (uVar123 & 0xff) << 0x10 | uVar108 & 0xff |
                            (uVar118 & 7) << 8);
  W(0x3e0,(uVar51 & 0xff) << 0xc | (uVar59 & 0x1ff) << 0x14 | (uVar52 & 0x1f) << 4
                            | uVar50);
  W(0x3e4,uVar59 & 0x1ff | 0x320000);
  iVar11 = ncu_img_w / 2;
  uVar10 = iVar11 / (int)uVar58;
  if (uVar58 == 0) {
  }
  iVar67 = ncu_img_h / 2;
  uVar65 = uVar58;
  if ((int)uVar58 < 0) {
    uVar65 = uVar58 + 0xf;
  }
  uVar65 = (uVar65 & 0xfff) >> 4;
  if (iVar11 % (int)uVar58 != 0) {
    uVar10 = uVar10 + 1;
  }
  uVar126 = iVar67 / (int)uVar58;
  if (uVar58 == 0) {
  }
  if (iVar67 % (int)uVar58 != 0) {
    uVar126 = uVar126 + 1;
  }
  W(1000,uVar65 << 0x10 | uVar65);
  W(0x3ec,0);
  W(0x3f0,local_250 | uVar23 * 0x10000000 | local_250 << 0x10);
  W(0x3f4,local_170 & 0x1fffff);
  uVar65 = (int)uVar58 % 0x10 & 0xf;
  W(0x3fc,uVar65 << 8 | uVar65);
  W(0x400,uVar126 & 0x3ff | (uVar10 & 0xff) << 0x10);
  W(0x3f8,uVar62 & 0xff | 0x8000 | (uVar63 & 3) << 0x10);
  W(0xc,5);
  return 0;

}
