"""Generate the original 135-slot client interface from pinned engine headers."""
import json
import re
import subprocess
from inputs import ROOT
from generate_bridge import ENGINE_REVISION, strip_comments


def main():
    upstream = ROOT / 'work/upstream/xash3d-fwgs'
    if subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=upstream, text=True).strip() != ENGINE_REVISION:
        raise RuntimeError('Client engine headers changed')
    source = strip_comments((upstream / 'engine/cdll_int.h').read_text())
    body = source.split('typedef struct cl_enginefuncs_s')[1].split('{', 1)[1].split('} cl_enginefunc_t;')[0]
    entries = []
    for raw in body.split(';'):
        raw = ' '.join(raw.split())
        if not raw:
            continue
        match = re.fullmatch(r'(.+?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)', raw)
        if match:
            entries.append(dict(zip(('returns', 'name', 'arguments'), match.groups()), kind='function'))
        else:
            match = re.fullmatch(r'struct (\w+)\s*\*\s*(\w+)', raw)
            if not match:
                raise ValueError(raw)
            entries.append(dict(name=match[2], typename=match[1], kind='table'))
    # The v3 Initialize export copies exactly 135 DWORDs; trailing engine
    # extensions must not silently change the guest layout.
    assert len(entries) == 135, len(entries)
    out = ROOT / 'work/bridge'
    out.mkdir(exist_ok=True, parents=True)
    header = '/* Generated client engine ABI. */\n'
    for i, entry in enumerate(entries):
        header += f'#define BHL_CL_{entry["name"]} {i}\n'
        header += f'static void client_engine_{i}(Ctx *c) {{ client_dispatch(c,{i}); }}\n'
    header += 'static const char *client_engine_names[] = {' + ','.join(json.dumps(e['name']) for e in entries) + '};\n'
    header += 'static void (*client_engine_callbacks[])(Ctx *) = {' + ','.join(f'client_engine_{i}' for i in range(len(entries))) + '};\n'
    header += 'static void fill_client_engine(uint32_t table) {\n'
    for i, e in enumerate(entries):
        if e['kind'] == 'function':
            header += f'rt_w32(G_MEM,table+{i*4},bhl_thunk("client-engine!{e["name"]}",client_engine_{i}));\n'
        else:
            header += f'rt_w32(G_MEM,table+{i*4},client_subtable("{e["name"]}",engine.{e["name"]}));\n'
    header += '}\n'
    # C callbacks require distinct host trampolines; a guest code address is
    # never cast to a native function pointer.
    for i in range(128):
        header += f'static void command_{i}(void) {{ run_command({i}); }}\n'
        header += f'static int message_{i}(const char *n,int size,void *data) {{ return run_message({i},n,size,data); }}\n'
        header += f'static void event_{i}(struct event_args_s *args) {{ run_event({i},args); }}\n'
    for kind, signature in [('command','void (*commands[])(void)'),('message','int (*messages[])(const char *,int,void *)'),('event','void (*events[])(struct event_args_s *)')]:
        header += f'static {signature} = {{' + ','.join(f'{kind}_{i}' for i in range(128)) + '};\n'
    (out / 'client_engine_abi.h').write_text(header)
    subheaders = dict(pTriAPI='triangleapi.h',pEfxAPI='r_efx.h',pEventAPI='event_api.h',pDemoAPI='demo_api.h',pNetAPI='net_api.h',pVoiceTweak='ivoicetweak.h')
    subcode = ''
    builders = ''
    for e in entries:
        if e['kind'] != 'table': continue
        text = strip_comments((upstream/'common'/subheaders[e['name']]).read_text())
        body = text.split('typedef struct '+e['typename'])[1].split('{',1)[1].split('}')[0]
        fields = [' '.join(raw.split()) for raw in body.split(';') if raw.strip()]
        builders += f'if(!strcmp(name,"{e["name"]}")) {{ uint32_t p=bhl_heap_alloc({len(fields)*4},1);\n'
        for i, raw in enumerate(fields):
            match = re.fullmatch(r'(.+?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)',raw)
            if match:
                name = e['name']+'_'+match[2]
                subcode += f'static void sub_{name}(Ctx *c) {{ client_missing("{e["name"]}.{match[2]}"); }}\n'
                builders += f'if(engine.{e["name"]}->{match[2]})rt_w32(G_MEM,p+{i*4},bhl_thunk("client-subtable!{name}",sub_{name}));\n'
            elif raw == 'int version':
                builders += f'rt_w32(G_MEM,p+{i*4},engine.{e["name"]}->version);\n'
            else: raise ValueError(raw)
        builders += 'return p; }\n'
    subcode += 'static uint32_t client_subtable(const char *name,const void *host) { if(!host)return 0;\n'+builders+'client_missing(name);return 0; }\n'
    (out/'client_subtables.h').write_text(subcode)
    implementation = (ROOT / 'runtime/client_bridge.c').read_text()
    manual = set(re.findall(r'case BHL_CL_(\w+)', implementation))
    scalars = []
    for e in entries:
        if e['kind'] != 'function' or e['name'] in manual or '...' in e['arguments'] or '(*' in e['arguments']:
            continue
        arguments = [] if e['arguments'].strip() == 'void' else e['arguments'].split(',')
        converted = []
        for i, arg in enumerate(arguments):
            arg = re.sub(r'(\w+)\s*\[[^]]*\]',r'* \1',arg)
            arg = ' '.join(arg.replace('*', ' * ').split())
            # Some upstream pointer parameters omit their variable name.
            kind = arg if arg.endswith('*') else arg.rsplit(' ', 1)[0]
            if kind in ('int','unsigned int','unsigned short','qboolean','HSPRITE'):
                converted.append(f'({kind})A({i})')
            elif kind == 'float':
                converted.append(f'rt_rf32(G_MEM,c->esp+{4+4*i})')
            elif kind in ('char *', 'const char *'):
                converted.append(f'gs(A({i}))')
            elif kind in ('float *','const float *','int *','const wrect_t *'):
                converted.append(f'({kind})(A({i})?G_MEM+A({i}):NULL)')
            else:
                break
        else:
            call = f'engine.{e["name"]}(' + ','.join(converted) + ')'
            returns = ' '.join(e['returns'].replace('*',' * ').split())
            if returns == 'void': statement = call + ';RET(0);'
            elif returns in ('int','unsigned int','unsigned short','qboolean','HSPRITE'): statement = 'RET('+call+');'
            elif returns in ('float','double'): statement = 'FPUSH('+call+');RET(0);'
            elif returns in ('char *','const char *'): statement = 'RET(copy_string('+call+'));'
            else: continue
            scalars.append((e['name'],f'case BHL_CL_{e["name"]}: {statement}\n'))
    (out / 'client_scalar_callbacks.h').write_text(''.join(s for _, s in scalars))
    # Export exact native signatures. Unsupported exports abort by name.
    declarations = strip_comments((upstream / 'engine/cdll_exp.h').read_text())
    declarations = declarations.split('typedef struct cldll_func_s')[1].split('{',1)[1].split('} cldll_func_t;')[0]
    export_types = {}
    for raw in declarations.split(';'):
        match = re.fullmatch(r'(.+?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)', ' '.join(raw.split()))
        if match: export_types[match[2]] = (match[1],match[3])
    mappings = re.findall(r'\{\s*"([^"]+)"\s*,\s*\(void \*\*\)&clgame.dllFuncs\.(\w+)', (upstream/'engine/client/dll_int/cl_game.c').read_text())
    original = __import__('pefile').PE(str(ROOT/'work/images/client.dll'))
    available = {e.name.decode() for e in original.DIRECTORY_ENTRY_EXPORT.symbols if e.name}
    implemented = set(re.findall(r'EXPORT\s+[^;\n(]*?[\s*](\w+)\s*\(', implementation))
    missing = ''
    for export, field in mappings:
        if export not in available or export in implemented: continue
        returns,args = export_types[field]
        missing += f'EXPORT {returns} {export}({args}) {{ client_missing("export: {export}"); __builtin_unreachable(); }}\n'
    (out/'client_missing_exports.h').write_text(missing)
    report = dict(slots=len(entries),implemented=sorted(manual | {n for n,_ in scalars}),pending=[e['name'] for e in entries if e['name'] not in manual | {n for n,_ in scalars}])
    (out/'client-coverage.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'Generated {len(entries)} client engine slots, {len(report["implemented"])} adapters (coverage is not validation)')


if __name__ == '__main__': main()
