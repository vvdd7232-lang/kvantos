"""Guest textedit.h layout (identical on i386 and x86-64)."""
import ctypes as C
CAP=65536
STATE_OFFSET=CAP*3
class Doc(C.Structure):
    _fields_=[('data',C.c_char*CAP),('start',C.c_ushort*CAP),
              ('ints',C.c_int*9),('used',C.c_uint),('width',C.c_int)]
class Undo(C.Structure):
    _fields_=[('doc',Doc),('valid',C.c_int)]
DOC_SIZE=C.sizeof(Doc)
MODE_OFFSET=DOC_SIZE+C.sizeof(Undo)+4
