import os
import sys
import socket

def run_srv(conn):
    data = conn.recv(9)

    parsed = data.decode().split(' ')
    cmd = parsed[0]
    arg = parsed[1]
    if cmd == 'get':
        if arg == 'vendor':
            conn.send('0005')

def setup_src(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM);
    s.bind(path)

    print('listen {}'.format(path))
    s.listen(1)
    conn, addr = s.accept()
    print('connected from {}'.format(addr))

    run_srv(conn)


if __name__ == '__main__':
    sockpath = "/tmp/qemu-epc-bridge.sock"
    try:
        setup_src(sockpath)
    except Exception as e:
        os.remove(sockpath)
        print('exception occurd {}'.format(e))
    print('done')


