"""Guest textedit.h layout (identical on i386 and x86-64)."""
import ctypes as C
CAP=65536
STATE_OFFSET=CAP*3
class Doc(C.Structure):
    _fields_=[('data',C.c_char*CAP),('start',C.c_ushort*CAP),
              ('ints',C.c_int*9),('used',C.c_uint),('width',C.c_int)]
class Mark(C.Structure):
    _fields_=[('position',C.c_int*7),('revision',C.c_uint)]
class Change(C.Structure):
    _fields_=[('range',C.c_uint*4),('before',Mark),('after',Mark)]
class Undo(C.Structure):
    _fields_=[('data',C.c_char*(CAP*2)),('changes',Change*64),('state',C.c_uint*6)]
DOC_SIZE=C.sizeof(Doc)
MODE_OFFSET=DOC_SIZE+C.sizeof(Undo)+4

class Entry(C.Structure):
    _fields_=[('name',C.c_char*128),('size',C.c_uint),('flags',C.c_ubyte*4),('year',C.c_ushort),('date',C.c_ubyte*4)]
def editor_type(is64):
    class Editor(C.Structure):
        _pack_=8 if is64 else 4
        _fields_=[('doc',Doc),('undo',Undo),('named',C.c_int),('mode',C.c_int),('pending',C.c_int),
                  ('disk',C.c_int),('top',C.c_int),('caret',C.c_int),('rows',C.c_int),('cols',C.c_int),
                  ('name',C.c_char*24),('status',C.c_char*192),('name_in',C.c_char*24),('pending_name',C.c_char*24),
                  ('until',C.c_uint64),('path',C.c_char*256),('file',C.c_char*256),('items',Entry*128),
                  ('count',C.c_int),('selected',C.c_int),('volume',C.c_int),
                  ('query',C.c_char*128),('replacement',C.c_char*128),('search_focus',C.c_int),('search_caret',C.c_int),
                  ('ram_revision',C.c_uint),('disk_revision',C.c_uint),('ram_valid',C.c_int),('disk_valid',C.c_int),
                  ('disk_failed',C.c_int),('imported',C.c_int),
                  ('target',C.c_char*256),('save_name',C.c_char*128),
                  ('target_epoch',C.c_uint),('target_usb',C.c_uint),('target_revision',C.c_uint),
                  ('save_epoch',C.c_uint),('save_usb',C.c_uint),('target_valid',C.c_int),('target_failed',C.c_int)]
    return Editor
