import struct
from mccard import *
CHAR_SIZE=0x8050
def dec(buf, size=CHAR_SIZE):
    b = bytearray(buf)
    ver, key, sm = struct.unpack_from("<HHH", b, 0)
    s=0; k=key
    for i in range((size-0x40)//2):
        o=0x40+2*i
        w=struct.unpack_from("<H",b,o)[0]^k
        struct.pack_into("<H",b,o,w); s=(s+w)&0xFFFF
        if k==0:k=1
        k=(k*176)%65363
    return bytes(b), ver, key, sm, s
def enc_same_head(plain, size=CHAR_SIZE):
    """re-encrypt with the head's version/key kept, the sum recomputed (the game's SaveFile_Encrypt with a fixed head)"""
    b = bytearray(plain)
    ver, key, _ = struct.unpack_from("<HHH", b, 0)
    s=0; k=key
    for i in range((size-0x40)//2):
        o=0x40+2*i
        w=struct.unpack_from("<H",b,o)[0]
        s=(s+w)&0xFFFF
        struct.pack_into("<H",b,o,w^k)
        if k==0:k=1
        k=(k*176)%65363
    struct.pack_into("<H",b,4,s)
    return bytes(b)
BODY=0x50
def box(body):
    out=[]
    for i in range(500):
        u,t,idn,lv,d0,d1,d2=struct.unpack_from("<BBHH3H",body,BODY*0+0xBC+i*12)
        out.append((u,t,idn,lv,d0,d1,d2))
    return out
