"""Generate CP1252 mappings from Microsoft's published Unicode mapping data."""
import hashlib
from inputs import ROOT


def generate(destination):
    source = ROOT / 'vendor/nls/bestfit1252.txt'
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != '72ea23c939c5b26fae7aded0207b327e2f3902d7d3c168d7087f5cfc38ee76a9':
        raise RuntimeError('CP1252 mapping input changed')
    tables, current = {}, None
    for line in data.decode('ascii').splitlines():
        fields = line.split(';')[0].split()
        if not fields:
            continue
        if fields[0] in ('MBTABLE', 'WCTABLE'):
            current = fields[0]
            tables[current] = []
        elif current and fields[0].startswith('0x'):
            tables[current].append(tuple(int(x, 16) for x in fields[:2]))
    assert len(tables['MBTABLE']) == 256 and len(tables['WCTABLE']) == 698
    text = '/* Unicode mapping data; see vendor/nls/LICENSE.txt. */\n'
    text += 'static const uint16_t cp1252_decode[256] = {\n'
    text += ','.join(hex(value) for key, value in sorted(tables['MBTABLE'])) + '};\n'
    text += 'static const struct { uint16_t unicode; uint8_t byte; } cp1252_encode[] = {\n'
    text += ''.join('{%s,%s},\n' % (hex(key), hex(value)) for key, value in sorted(tables['WCTABLE'])) + '};\n'
    destination.write_text(text)
