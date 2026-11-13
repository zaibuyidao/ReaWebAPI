"""Original deterministic fixtures for input, animation and state restoration."""


def dos_program():
    code = bytearray.fromhex('b8 13 00 cd 10 68 00 a0 07 b0 b6 e6 43 b8 98 0a e6 42 88 e0 e6 42 e4 61 0c 03 e6 61 bb 02 00')
    labels, branches = {}, []

    def emit(value): code.extend(bytes.fromhex(value))
    def branch(op, label):
        emit(op + ' 00')
        branches.append((len(code) - 1, label))

    labels['frame'] = len(code)
    emit('31 ff b9 00 fa 88 d8 f3 aa bf c0 f8 b9 40 01 88 f8 f3 aa fe c7 b4 01 cd 16')
    branch('74', 'vblank')
    emit('b4 00 cd 16 0c 20 3c 61')
    branch('75', 'key_b')
    emit('b3 04')
    branch('eb', 'vblank')
    labels['key_b'] = len(code)
    emit('3c 62')
    branch('75', 'vblank')
    emit('b3 02')
    labels['vblank'] = len(code)
    emit('ba da 03 ec a8 08 75 fb ec a8 08 74 fb')
    branch('eb', 'frame')
    for offset, label in branches:
        distance = labels[label] - offset - 1
        assert -128 <= distance <= 127
        code[offset] = distance & 255
    return code


def nes_program():
    code, labels, branches, jumps = bytearray(), {}, [], []

    def emit(value): code.extend(bytes.fromhex(value))
    def branch(op, label):
        emit(op + ' 00'); branches.append((len(code) - 1, label))
    def jump(label):
        emit('4c 00 00'); jumps.append((len(code) - 2, label))

    emit('78 d8 a2 ff 9a e8 8e 00 20 8e 01 20 86 00 86 01')
    labels['frame'] = len(code)
    emit('2c 02 20')
    branch('10', 'frame')
    emit('e6 00 a9 01 8d 16 40 a9 00 8d 16 40 ad 16 40 29 01')
    branch('f0', 'key_b')
    emit('a9 01 85 01')
    labels['key_b'] = len(code)
    emit('ad 16 40 29 01')
    branch('f0', 'render')
    emit('a9 00 85 01')
    labels['render'] = len(code)
    emit('a9 3f 8d 06 20 a9 00 8d 06 20 a5 01')
    branch('f0', 'green')
    emit('a9 06')
    jump('color')
    labels['green'] = len(code)
    emit('a9 0a')
    labels['color'] = len(code)
    emit('85 02 a5 00 29 10 05 02 8d 07 20')
    labels['wait'] = len(code)
    emit('2c 02 20')
    branch('30', 'wait')
    jump('frame')
    for offset, label in branches: code[offset] = (labels[label] - offset - 1) & 255
    for offset, label in jumps: code[offset:offset + 2] = (0x8000 + labels[label]).to_bytes(2, 'little')
    prg = bytearray([0xea] * 16384)
    prg[:len(code)] = code
    prg[-6:] = bytes.fromhex('00 80 00 80 00 80')
    return b'NES\x1a\x01\x01' + bytes(10) + prg + bytes(8192)
