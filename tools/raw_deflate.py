import ctypes as c

class Stream(c.Structure):
    _fields_ = [('next_in',c.c_void_p),('avail_in',c.c_uint),('total_in',c.c_ulong),
                ('next_out',c.c_void_p),('avail_out',c.c_uint),('total_out',c.c_ulong),
                ('msg',c.c_char_p),('state',c.c_void_p),('zalloc',c.c_void_p),
                ('zfree',c.c_void_p),('opaque',c.c_void_p),('data_type',c.c_int),
                ('adler',c.c_ulong),('reserved',c.c_ulong)]

def compress_raw(dll, data):
    assert len(data)<100*1024*1024
    lib=c.CDLL(str(dll))
    lib.zlibVersion.restype=c.c_char_p
    lib.deflateInit2_.argtypes=[c.POINTER(Stream),c.c_int,c.c_int,c.c_int,c.c_int,c.c_int,c.c_char_p,c.c_int]
    lib.deflateBound.argtypes=[c.POINTER(Stream),c.c_ulong]
    lib.deflateBound.restype=c.c_ulong
    lib.deflate.argtypes=[c.POINTER(Stream),c.c_int]
    lib.deflateEnd.argtypes=[c.POINTER(Stream)]
    stream=Stream()
    assert lib.deflateInit2_(c.byref(stream),6,8,-15,8,0,lib.zlibVersion(),c.sizeof(Stream))==0
    try:
        capacity=lib.deflateBound(c.byref(stream),len(data))
        assert len(data)<=capacity<110*1024*1024
        incoming=c.create_string_buffer(data)
        outgoing=c.create_string_buffer(capacity)
        stream.next_in=c.addressof(incoming)
        stream.avail_in=len(data)
        stream.next_out=c.addressof(outgoing)
        stream.avail_out=capacity
        assert lib.deflate(c.byref(stream),4)==1 and stream.avail_in==0
        return outgoing.raw[:stream.total_out]
    finally:
        assert lib.deflateEnd(c.byref(stream))==0
