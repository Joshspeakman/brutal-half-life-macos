"""Generate checked engine ABI declarations from the pinned upstream headers."""
import json
import re
import subprocess
import pefile
from inputs import ROOT

ENGINE_REVISION = "fd0cd6afdda9dff94346af60884b99876001ca5f"


def strip_comments(source):
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)


def declarations(body):
    # The outer declaration ends at its semicolon; nested callback arguments
    # must not be counted as additional ABI slots.
    entries = []
    for declaration in body.split(';'):
        declaration = ' '.join(declaration.split())
        match = re.fullmatch(r'(.+?)\(\s*\*\s*(pfn\w+)\s*\)\s*\((.*)\)', declaration)
        if not match:
            if declaration:
                raise ValueError(f'Unparsed interface declaration: {declaration}')
            continue
        entries.append(match.groups())
    return entries


def guest_struct(source, typename, guestname):
    body=source[:source.index('} '+typename+';')].rsplit('typedef struct',1)[1].split('{',1)[1]
    fields=[]; converted=[]
    for raw in body.split(';'):
        raw=' '.join(raw.split())
        if not raw:continue
        callback=re.fullmatch(r'.+?\(\s*\*\s*(\w+)\s*\)\s*\(.*\)',raw)
        if callback:
            name=callback[1];converted.append('uint32_t '+name+';');fields.append((name,'callback'));continue
        if '*' in raw:
            name=raw.rsplit('*',1)[1].strip()
            if not re.fullmatch(r'\w+',name):raise ValueError(raw)
            converted.append('uint32_t '+name+';');fields.append((name,'pointer'));continue
        converted.append(raw.replace('physent_t','BhlPhysent')+';')
        for field in raw.split(','):
            name=re.search(r'(\w+)\s*(?:\[.*\])?$',field)[1]
            fields.append((name,'physents' if 'physent_t' in raw else 'string' if raw.startswith('string_t ') else 'value'))
    return 'typedef struct {\n'+'\n'.join(converted)+'\n} '+guestname+';\n',fields


def main():
    upstream = ROOT / 'work/upstream/xash3d-fwgs'
    revision = subprocess.check_output(['git','rev-parse','HEAD'],cwd=upstream,text=True).strip()
    if revision != ENGINE_REVISION:
        raise RuntimeError('Engine headers changed; audit the ABI before updating the pinned revision')
    source = strip_comments((upstream/'engine/eiface.h').read_text())
    engine = declarations(source.split('typedef struct enginefuncs_s')[1].split('{',1)[1].split('} enginefuncs_t;')[0])
    server_body = source[:source.index('} DLL_FUNCTIONS;')].rsplit('typedef struct',1)[1].split('{',1)[1]
    server = declarations(server_body)
    assert len(engine)==159 and len(server)==50,(len(engine),len(server))
    out = ROOT/'work/bridge'
    out.mkdir(parents=True,exist_ok=True)
    header='/* Generated from pinned Xash3D declarations; implementation lives in server_bridge.c. */\n'
    for i,(_,name,_) in enumerate(engine):header+=f'#define BHL_ENG_{name} {i}\n'
    for i,(_,name,_) in enumerate(server):header+=f'#define BHL_DLL_{name} {i}\n'
    header+='static const char *engine_names[] = {\n'+''.join(json.dumps(name)+',\n' for _,name,_ in engine)+'};\n'
    header+='static const char *server_names[] = {\n'+''.join(json.dumps(name)+',\n' for _,name,_ in server)+'};\n'
    for i,(ret,name,args) in enumerate(server):
        header+=f'static {ret} missing_{name}({args}) {{ bridge_missing("server",{i}); __builtin_unreachable(); }}\n'
    header+='static void fill_missing(DLL_FUNCTIONS *table) {\n'
    header+=''.join(f'table->{name}=missing_{name};\n' for _,name,_ in server)+'}\n'
    for i in range(len(engine)):header+=f'static void engine_{i}(Ctx *c) {{ engine_dispatch(c,{i}); }}\n'
    header+='static void (*engine_callbacks[])(Ctx *) = {\n'+''.join(f'engine_{i},\n' for i in range(len(engine)))+'};\n'
    (out/'engine_abi.h').write_text(header)
    movement=strip_comments((upstream/'pm_shared/pm_defs.h').read_text())
    phys,physfields=guest_struct(movement,'physent_t','BhlPhysent')
    pm,fields=guest_struct(movement,'playermove_t','BhlPlayerMove')
    guest=phys+pm+'\n_Static_assert(sizeof(BhlPhysent)==224,"guest physent layout");\n'
    guest+='static void pm_copy_values(BhlPlayerMove *g,const playermove_t *h) {\n'
    guest+=''.join(f'memcpy(&g->{n},&h->{n},sizeof g->{n});\n' for n,k in fields if k=='value')+'}\n'
    guest+='static void pm_write_values(playermove_t *h,const BhlPlayerMove *g) {\n'
    guest+=''.join(f'memcpy(&h->{n},&g->{n},sizeof h->{n});\n' for n,k in fields if k=='value')+'}\n'
    for direction in ('guest','host'):
        dst,src=('g','h') if direction=='guest' else ('h','g')
        guest+=('static void phys_to_guest(BhlPhysent *g,const physent_t *h) {\n' if direction=='guest' else 'static void phys_to_host(physent_t *h,const BhlPhysent *g) {\n')
        for name,kind in physfields:
            if kind=='value':guest+=f'memcpy(&{dst}->{name},&{src}->{name},sizeof {dst}->{name});\n'
            else:guest+=f'{dst}->{name}=model_{direction}({src}->{name});\n'
        guest+='}\n'
    pm_callbacks=[n for n,k in fields if k=='callback']
    for i,n in enumerate(pm_callbacks):
        guest+=f'#define BHL_PM_{n} {i}\nstatic void pm_{i}(Ctx *c) {{ pm_dispatch(c,{i}); }}\n'
    guest+='static const char *pm_names[] = {'+','.join(json.dumps(n) for n in pm_callbacks)+'};\n'
    guest+='static void pm_fill_callbacks(BhlPlayerMove *g) {\n'
    guest+=''.join(f'g->{n}=bhl_thunk(BHL_PM_NAMESPACE "{n}",pm_{i});\n' for i,n in enumerate(pm_callbacks))+'}\n'
    (out/'guest_movement.h').write_text(guest)
    progdefs=strip_comments((upstream/'engine/progdefs.h').read_text())
    ent,fields=guest_struct(progdefs,'entvars_t','BhlEntvars')
    entities=ent+'''typedef struct {
int free,serialnumber; uint32_t area[2]; int headnode,num_leafs;
short leafnums[48]; float freetime; uint32_t pvPrivateData; BhlEntvars v;
} BhlEdict;
_Static_assert(sizeof(BhlEntvars)==676,"guest entvars layout");
_Static_assert(sizeof(BhlEdict)==804,"guest edict layout");
_Static_assert(offsetof(BhlEdict,v)==128,"guest edict entvars offset");
'''
    for direction in ('guest','host'):
        entities+=('static void copy_entvars_to_guest(BhlEntvars *g,const entvars_t *h) {\n' if direction=='guest'
                   else 'static void copy_entvars_to_host(entvars_t *h,const BhlEntvars *g) {\n')
        for name,kind in fields:
            # GoldSrc declares these as int, but their values are string_t IDs.
            if name in ('viewmodel','weaponmodel'):kind='string'
            dst,src=('g','h') if direction=='guest' else ('h','g')
            if kind=='value':entities+=f'memcpy(&{dst}->{name},&{src}->{name},sizeof {dst}->{name});\n'
            else:entities+=f'{dst}->{name}={direction}_{"edict" if kind=="pointer" else "string"}({src}->{name});\n'
        entities+='}\n'
    (out/'guest_entities.h').write_text(entities)
    pe=pefile.PE(str(ROOT/'work/images/hl.dll'))
    wrappers=''
    excluded={'GetEntityAPI','GetEntityAPI2','GetNewDLLFunctions','GiveFnptrsToDll'}
    for export in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if export.name and re.fullmatch(b'[A-Za-z_][A-Za-z_0-9]*',export.name) and export.name.decode() not in excluded:
            wrappers+=f'__attribute__((visibility("default"))) void {export.name.decode()}(entvars_t *pev) {{ link_entity(0x{pe.OPTIONAL_HEADER.ImageBase+export.address:08x}u,pev); }}\n'
    (out/'entity_exports.h').write_text(wrappers)
    implemented=set(re.findall(r'case BHL_ENG_(\w+)',(ROOT/'runtime/server_bridge.c').read_text()))
    automatic=[]; bridged=[]
    for returns,name,arguments in engine:
        if name in implemented or '...' in arguments or '(*' in arguments:continue
        expressions=[];valid=True
        arguments=[] if arguments.strip()=='void' else arguments.split(',')
        for i,arg in enumerate(arguments):
            match=re.fullmatch(r'(.+?)[\s*]+([A-Za-z_]\w*)',arg.strip())
            if not match:valid=False;break
            kind=arg.strip()[:match.start(2)].strip()
            kind=' '.join(kind.replace('*',' * ').split())
            if kind in ('int','unsigned int','unsigned short','short','unsigned char','byte','qboolean','FORCE_TYPE'):
                expressions.append(f'({kind})A({i})')
            elif kind=='float':expressions.append(f'rt_rf32(G_MEM,c->esp+{4+4*i})')
            elif kind in ('char *','const char *'):expressions.append(f'gs(A({i}))')
            elif kind in ('edict_t *','const edict_t *'):expressions.append(f'host_edict(A({i}))')
            elif kind in ('float *','const float *','int *','unsigned int *','unsigned char *'):
                expressions.append(f'(A({i})?({kind})(G_MEM+A({i})):NULL)')
            else:valid=False;break
        if not valid:continue
        ret=' '.join(returns.replace('*',' * ').split())
        call=f'engine.{name}('+','.join(expressions)+')'
        if ret=='void':statement=call+';RET(0);'
        elif ret in ('int','unsigned int','unsigned short','qboolean'):statement='RET('+call+');'
        elif ret=='float':statement='FPUSH('+call+');RET(0);'
        elif ret in ('const char *','char *'):statement='RET(copy_string('+call+'));'
        elif ret=='edict_t *':statement='RET(guest_edict('+call+'));'
        else:continue
        automatic.append(f'case BHL_ENG_{name}: {statement}\n');bridged.append(name)
    (out/'engine_scalar_callbacks.h').write_text(''.join(automatic))
    manual_server=set(re.findall(r'out->(pfn\w+)\s*=',(ROOT/'runtime/server_bridge.c').read_text()))
    server_wrappers=''; assignments=[]
    for i,(returns,name,arguments) in enumerate(server):
        if name in manual_server or returns.strip()!='void':continue
        args=[] if arguments.strip()=='void' else arguments.split(',')
        expressions=[]
        for arg in args:
            match=re.fullmatch(r'(?:const\s+)?edict_t\s*\*\s*(\w+)',arg.strip())
            if not match:break
            expressions.append(f'guest_edict({match[1]})')
        else:
            count=len(expressions)
            server_wrappers+=f'static void scalar_{name}({arguments}) {{ '
            server_wrappers+=(('uint32_t args[]={'+','.join(expressions)+'}; ') if count else '')
            server_wrappers+=f'server_call({i},{"args" if count else "NULL"},{count}); }}\n'
            assignments.append(f'out->{name}=scalar_{name};\n')
    server_wrappers+='static void fill_scalar_server(DLL_FUNCTIONS *out) {\n'+''.join(assignments)+'}\n'
    (out/'server_scalar_callbacks.h').write_text(server_wrappers)
    (out/'callback-coverage.json').write_text(json.dumps(dict(
        implemented_engine=sorted(implemented|set(bridged)),
        pending_engine=[n for _,n,_ in engine if n not in implemented and n not in bridged],
        gameplay_validated=False),indent=2)+'\n')
    (out/'abi.json').write_text(json.dumps(dict(engine_revision=revision,
        engine=[dict(slot=i,returns=r,name=n,arguments=a) for i,(r,n,a) in enumerate(engine)],
        server=[dict(slot=i,returns=r,name=n,arguments=a) for i,(r,n,a) in enumerate(server)]),indent=2)+'\n')
    print('Verified 159 engine slots and 50 server slots')


if __name__=='__main__':main()
