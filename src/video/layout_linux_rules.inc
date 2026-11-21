// Linux x86_64 video ABI derived from REAPER 7.86 ELF unwind ranges.
// SHA-256 retains opcodes, register use, in-function branches and member offsets.
// Listed disp32/rel32 operands and the three independently checked effect fields are zeroed.
enum class Role {
  display, query, source_hash, cached, destroy_sources, transform,
  yuv_gamut, sources_at, window_state, window_visibility, frame_state, color,
  parameter, factory, visible, window, frame, processed,
  monitor_hash, frame_index, is_visible, converter, codec_version,
  count
};
constexpr uint16_t operands[]{
  19, 32, 49, 60, 81, 96, 110, 121, 133, 162, 169, 177, 197, 210,
  218, 231, 243, 250, 255, 261, 294, 302, 310, 339, 367, 411, 463, 551,
  604, 666, 695, 732, 749, 757, 778, 797, 815, 826, 834, 841, 862, 884,
  890, 909, 919, 957, 967, 1002, 1014, 1036, 1044, 1059, 1068, 1079, 1092, 1124,
  1143, 1148, 1156, 1189, 1198, 1207, 1220, 1238, 1249, 1276, 1339, 1363, 1387, 1420,
  1477, 1483, 1490, 1501, 1519, 1524, 1531, 1536, 1543, 1560, 1567, 1579, 1596, 1606,
  1621, 1637, 1661, 1669, 1692, 1699, 1731, 1736, 1751, 1758, 1768, 1778, 1788, 1797,
  1804, 1811, 1818, 1823, 1855, 1863, 1885, 1907, 1919, 1959, 1971, 2001, 2014, 2031,
  2065, 2080, 2094, 2100, 2116, 2168, 2173, 2195, 2200, 2215, 2222, 2232, 2242, 2252,
  2262, 2269, 2275, 2282, 2289, 2296, 2301, 2323, 2328, 2343, 2350, 2360, 2370, 2380,
  2390, 2397, 2403, 2410, 2417, 2424, 2429, 2459, 2464, 2479, 2486, 2496, 2506, 2516,
  2525, 2532, 2539, 2546, 2551, 2579, 2587, 2638, 2669, 2708, 2750, 2787, 2801, 2820,
  2848, 2871, 2896, 2909, 2922, 2930, 2937, 2961, 2974, 2981, 2995, 3018, 3031, 3048,
  3053, 3060, 3065, 3077, 3105, 3120, 3135, 3143, 3154, 3170, 3190, 3197, 3219, 3226,
  3248, 3264, 3278, 3284, 3292, 3299, 3305, 3311, 3336, 3353, 3361, 3367, 3410, 3427,
  3446, 3460, 3472, 3517, 3532, 3550, 3598, 3606, 3612, 3723, 3747, 3769, 3806, 3852,
  3874, 3906, 3925, 4156, 4355, 4382, 4394, 4475, 4619, 4627, 43, 56, 65, 107,
  123, 158, 205, 268, 299, 307, 313, 365, 393, 452, 465, 501, 518, 529,
  590, 599, 610, 618, 626, 645, 660, 691, 696, 728, 739, 767, 775, 791,
  841, 914, 988, 996, 1012, 1083, 1109, 1116, 1130, 1152, 1176, 1198, 1214, 1222,
  1242, 1253, 1352, 1402, 1416, 1495, 1518, 1528, 1579, 1588, 1599, 1627, 1646, 1651,
  1670, 1686, 1705, 1751, 24, 31, 140, 305, 701, 30, 37, 48, 54, 61,
  79, 93, 98, 267, 272, 10, 24, 35, 43, 66, 94, 149, 243, 278,
  3, 14, 73, 86, 102, 107, 113, 179, 215, 231, 260, 265, 278, 365,
  410, 415, 428, 451, 507, 540, 563, 971, 988, 999, 1083, 1107, 1112, 1124,
  1426, 1556, 1563, 1766, 1792, 1845, 1863, 1898, 1909, 1924, 1962, 1970, 1982, 1990,
  3, 12, 21, 38, 28, 40, 52, 83, 97, 15, 30, 92, 120, 154,
  182, 191, 214, 234, 241, 276, 299, 339, 2, 12, 53, 73, 81, 89,
  97, 105, 113, 121, 129, 63, 80, 127, 19, 28, 42, 106, 114,
};
constexpr struct { uint16_t offset, next; Role target; } refs[]{
  {162, 4, Role::query}, {757, 4, Role::source_hash}, {778, 4, Role::cached},
  {834, 4, Role::source_hash}, {841, 4, Role::processed}, {862, 4, Role::frame},
  {909, 8, Role::processed}, {1156, 4, Role::yuv_gamut}, {1477, 4, Role::processed},
  {1490, 4, Role::monitor_hash}, {1543, 4, Role::processed}, {1579, 4, Role::frame},
  {1606, 4, Role::yuv_gamut}, {1669, 4, Role::frame}, {1692, 4, Role::frame},
  {1699, 4, Role::frame_index}, {1863, 4, Role::source_hash}, {1885, 4, Role::cached},
  {2296, 4, Role::destroy_sources}, {2424, 4, Role::destroy_sources}, {2801, 4, Role::frame},
  {2930, 4, Role::source_hash}, {2937, 5, Role::processed}, {2961, 4, Role::monitor_hash},
  {2981, 4, Role::processed}, {3190, 4, Role::frame}, {3219, 4, Role::processed},
  {3226, 8, Role::frame}, {3248, 8, Role::processed}, {3550, 4, Role::transform},
  {3769, 4, Role::processed}, {4382, 4, Role::processed}, {4394, 4, Role::frame},
  {4475, 4, Role::processed}, {268, 4, Role::sources_at}, {24, 4, Role::yuv_gamut},
  {243, 4, Role::yuv_gamut}, {3, 5, Role::window}, {12, 5, Role::visible},
  {28, 4, Role::window}, {40, 4, Role::is_visible}, {83, 4, Role::window},
  {97, 4, Role::is_visible}, {15, 4, Role::processed}, {214, 4, Role::frame_index},
  {299, 4, Role::frame}, {2, 5, Role::converter}, {12, 4, Role::codec_version},
};
constexpr struct { Role role; size_t size, mask_begin, mask_count, ref_begin, ref_count; const char* digest; } rules[]{
  {Role::display, 4631, 0, 234, 0, 34, "765dc8e5b3c9d744d4a0f7e3cb79782b2752c00912a699e5232320fc90f665d7"},
  {Role::query, 1764, 234, 64, 34, 1, "e5ba91e7ca07e6acd6b20a604d4de14f9625a4a8c1df16643e906dce788df57f"},
  {Role::source_hash, 810, 298, 5, 35, 1, "b99a081e4aaf2c95f02b35d396412af2d3f8119690ca3cc3928d02a31f204a3d"},
  {Role::cached, 322, 303, 10, 36, 0, "704dbad8dea245f8a6bfebbb60454f2002165a3b95e3897631194cb3a2fef487"},
  {Role::destroy_sources, 47, 313, 4, 36, 0, "921db6df144e6a87acb9a203432da17963e59d41bf46f696db5bebf7c6126c92"},
  {Role::transform, 326, 317, 5, 36, 1, "c230c7b08c28ab43cba06b675cd382f6b8f2e5a113dc9dbf20b523eb4b407dec"},
  {Role::yuv_gamut, 60, 322, 2, 37, 0, "d8255d13343688ae4411dc884aeaa9b38d6c1b31b5fad4ff64fb26e2cffacd28"},
  {Role::sources_at, 1999, 324, 40, 37, 0, "14f5ddef2de99d7c1b66ab74acc5f7364a42ac8e173e28746290b1496ae3b46d"},
  {Role::window_state, 42, 364, 4, 37, 2, "9e705f256fd6e4c2dc98944a7ca1422209e14d94e206c7ff8237e06381728a3c"},
  {Role::window_visibility, 111, 368, 5, 39, 4, "5c4da54dc2662fcc3608d6f10eb4abf12d08ecf4061cc93bd57526e323229ce3"},
  {Role::frame_state, 345, 373, 13, 43, 3, "70dbd63a4fc9c14538ef5b0d4fbb74e73a3b7918aa358c9284e9b2b55af51721"},
  {Role::color, 133, 386, 11, 46, 2, "d576ead107a7cf2073d34672a8897e8d34437c0d7aedbc286f8a6aed2124cd31"},
  {Role::parameter, 165, 397, 3, 48, 0, "26cf7228133d6832800ac8d4e9738a8ddc94e234bfa0fda925c98ac7111c25d0"},
  {Role::factory, 118, 400, 5, 48, 0, "59d37afe8055f4233a12f5c1e9804227142d6329b402211194848919f55fb1c8"},
};
constexpr size_t display_operand = 551;
constexpr size_t parameter_operand = 63;
constexpr size_t factory_operand = 19;
