"""QEMU debugger breakpoints, without any product/lab instrumentation or writes.
Unix RSP only. CPU is stopped at a normal function entry, then resumed or killed.
"""
import socket

class Breakpoint:
    def __init__(self,q,path,symbol):
        self.q=q;self.at=q.symbols[symbol];self.s=socket.socket(socket.AF_UNIX)
        q.cmd('stop');self.s.connect(str(path));self.s.settimeout(60)
        self.request('?');assert self.request(f'Z0,{self.at:x},1')==b'OK'
        self.s.sendall(b'$c#63');assert self.s.recv(1)==b'+'
    def packet(self):
        data=b''
        while not data.endswith(b'#'):
            b=self.s.recv(1)
            if not b:raise RuntimeError('debugger disconnected')
            if not data:
                if b==b'$':data=b'$'
            else:data+=b
        checksum=b''
        while len(checksum)<2:
            chunk=self.s.recv(2-len(checksum))
            if not chunk:raise RuntimeError('debugger disconnected in checksum')
            checksum+=chunk
        assert len(checksum)==2 and int(checksum,16)==sum(data[1:-1])%256,data
        self.s.sendall(b'+');return data[1:-1]
    def request(self,text):
        b=text.encode();self.s.sendall(b'$'+b+b'#'+f'{sum(b)%256:02x}'.encode())
        return self.packet()
    def wait(self):
        reply=self.packet();assert reply.startswith((b'T05',b'S05')),reply
        return reply
    def resume(self):
        assert self.request(f'z0,{self.at:x},1')==b'OK'
        assert self.request('D')==b'OK';self.s.close()
    def close(self):self.s.close()
