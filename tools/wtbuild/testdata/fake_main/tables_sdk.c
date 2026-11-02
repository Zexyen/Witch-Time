/* tables test, sdk side: data symbols main imports, a helper main imports (a host cut when lifted), and padding
 * that makes the stand-in as large as the addresses tables.rs probes */
void ext_log(int);
const unsigned char util_data[13] __asm__("_ZN2nn4util9kFakeDataE") = {1, 2, 3, 250, 0, 7, 9, 11, 13, 17, 19, 23, 255};
const unsigned char npad_ids[8] __asm__("_ZN2nn3hid6NpadId4FakeE") = {0, 1, 2, 3, 4, 5, 6, 0x20};
const unsigned char os_data[4] __asm__("_ZN2nn2os8kNotDataE") = {9, 9, 9, 9};
const unsigned char big_data[(1 << 20) + 8] __asm__("_ZN2nn4util8kTooBigE") = {1};
const unsigned char padding[0x200000] = {1};
int sdk_helper(int x) { ext_log(x); return x * 3; }
int sdk_uses_helper(int x) { return sdk_helper(x + 1) + sdk_helper(x - 1); }
