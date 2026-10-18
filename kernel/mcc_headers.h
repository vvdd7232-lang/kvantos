/* Read-only virtual headers for MCC, not host/kernel filesystem access.
   Each is included once per compile; constants/types consume ordinary budgets. */
static const struct {const char *name,*body;} pp_headers[]={
    {"kvui.h", "#include \"kvkeys.h\"\n#include \"kvtypes.h\"\n"
        "#define KV_BG 0x182032\n#define KV_FG 0xe6f0ff\n#define KV_TRANSPARENT -1\n"},
    {"kvkeys.h", "#define KV_KEY_UP 0x100\n#define KV_KEY_DOWN 0x101\n"
        "#define KV_KEY_LEFT 0x102\n#define KV_KEY_RIGHT 0x103\n"
        "#define KV_KEY_ENTER 10\n#define KV_KEY_ESC 27\n#define KV_KEY_BKSP 8\n#define KV_KEY_TAB 9\n"
        "#define KV_CHAR_W 8\n#define KV_CHAR_H 16\n"},
    {"kvtypes.h", "typedef int kv_int;\ntypedef char kv_byte;\n"
        "typedef struct kv_point {int x;int y;} kv_point;\n"},
#ifdef MCC_PP_TEST_HEADERS
    /* Available ONLY in the dedicated host guard test binary. */
    {"cycle1.h", "#include \"cycle2.h\"\n"},{"cycle2.h", "#include \"cycle1.h\"\n"},
    {"depth1.h", "#include \"depth2.h\"\n"},{"depth2.h", "#include \"depth3.h\"\n"},
    {"depth3.h", "#include \"depth4.h\"\n"},{"depth4.h", "#include \"depth5.h\"\n"},
    {"depth5.h", "typedef int deep_int;\n"},
#endif
};
#define PP_HEADERS (sizeof(pp_headers)/sizeof(pp_headers[0]))
#define PP_INCLUDE_DEPTH 4
static unsigned pp_included,pp_including;
_Static_assert(PP_HEADERS<=32,"include bitsets");
