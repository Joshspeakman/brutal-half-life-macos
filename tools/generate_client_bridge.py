"""Generate the original 135-slot client interface from pinned engine headers."""
import json
import re
import subprocess
from inputs import ROOT
from generate_bridge import ENGINE_REVISION, strip_comments


def scalar_body(returns, arguments, callee):
    if '...' in arguments or '(*' in arguments:return None
    converted=[];word=0
    for arg in ([] if arguments.strip()=='void' else arguments.split(',')):
        arg=re.sub(r'(\w+)\s*\[[^]]*\]',r'* \1',arg)
        arg=' '.join(arg.replace('*',' * ').split())
        kind=arg if arg.endswith('*') else arg.rsplit(' ',1)[0]
        if kind in ('int','const int','unsigned int','unsigned short','short','unsigned char','char','qboolean','HSPRITE','TRICULLSTYLE'):
            converted.append(f'({kind})A({word})')
        elif kind=='float':converted.append(f'rt_rf32(G_MEM,c->esp+{4+word*4})')
        elif kind in ('char *','const char *'):converted.append(f'gs(A({word}))')
        elif kind in ('float *','const float *','int *','short *','const wrect_t *','unsigned char *','const unsigned char *','byte *','const byte *','struct pmtrace_s *'):
            converted.append(f'({kind})(A({word})?G_MEM+A({word}):NULL)')
        else:return None
        word+=1
    call=callee+'('+','.join(converted)+')'
    returns=' '.join(returns.replace('*',' * ').split())
    if returns=='void':return call+';RET(0);'
    if returns in ('int','unsigned int','unsigned short','short','qboolean','HSPRITE'):return 'RET('+call+');'
    if returns in ('float','double'):return 'FPUSH('+call+');RET(0);'
    if returns in ('char *','const char *'):return 'RET(borrowed_string('+call+'));'
    if returns=='particle_t *':return 'RET(particle_guest('+call+'));'
    if returns=='BEAM *':return 'RET(beam_guest('+call+'));'
    return None


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
    for i in range(128):
        subcode+=f'static void projectile_{i}(TEMPENTITY *h,pmtrace_t *t){{projectile_hit({i},h,t);}}\n'
        subcode+=f'static void tracer_{i}(particle_t *h){{tracer_death({i},h);}}\n'
    subcode+='static void (*projectile_callbacks[])(TEMPENTITY *,pmtrace_t *)={'+','.join(f'projectile_{i}' for i in range(128))+'};\n'
    subcode+='static void (*tracer_callbacks[])(particle_t *)={'+','.join(f'tracer_{i}' for i in range(128))+'};\n'
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
                body=scalar_body(match[1],match[3],f'engine.{e["name"]}->{match[2]}')
                if name=='pTriAPI_SpriteTexture':body='RET(engine.pTriAPI->SpriteTexture(model_host(A(0)),A(1)));'
                if name in ('pEfxAPI_CL_AllocDlight','pEfxAPI_CL_AllocElight'):body=f'RET(light_guest(engine.{e["name"]}->{match[2]}(A(0))));'
                if name=='pEfxAPI_R_AllocParticle':body='RET(particle_alloc(A(0)));'
                if name=='pEfxAPI_CL_TentEntAllocCustom':body='RET(temp_custom_alloc((float*)gs(A(0)),model_host(A(1)),A(2),A(3)));'
                if name=='pEfxAPI_R_Projectile':body='engine.pEfxAPI->R_Projectile((float*)gs(A(0)),(float*)gs(A(1)),A(2),A(3),A(4),projectile_callback(A(5)));RET(0);'
                if name=='pEfxAPI_R_UserTracerParticle':body='engine.pEfxAPI->R_UserTracerParticle((float*)gs(A(0)),(float*)gs(A(1)),rt_rf32(G_MEM,c->esp+12),A(3),rt_rf32(G_MEM,c->esp+20),A(5),tracer_callback(A(6)));RET(0);'
                if name=='pEventAPI_EV_PlaybackEvent':body='playback_event(c,1);RET(0);'
                if name=='pEventAPI_EV_PlayerTraceExt':body='event_trace_ext(c);RET(0);'
                if name in ('pEventAPI_EV_GetPhysent','pEventAPI_EV_GetVisent'):body=f'RET(event_physent(engine.{e["name"]}->{match[2]}(A(0))));'
                if name in ('pEfxAPI_CL_TempEntAlloc','pEfxAPI_CL_TempEntAllocHigh'):body=f'RET(temp_guest(engine.{e["name"]}->{match[2]}((float*)gs(A(0)),model_host(A(1)))));'
                if name=='pEfxAPI_CL_TempEntAllocNoModel':body='RET(temp_guest(engine.pEfxAPI->CL_TempEntAllocNoModel((float*)gs(A(0)))));'
                if name=='pEfxAPI_R_TempModel':body='RET(temp_guest(engine.pEfxAPI->R_TempModel((float*)gs(A(0)),(float*)gs(A(1)),(float*)gs(A(2)),rt_rf32(G_MEM,c->esp+16),A(4),A(5))));'
                if name=='pEfxAPI_R_DefaultSprite':body='RET(temp_guest(engine.pEfxAPI->R_DefaultSprite((float*)gs(A(0)),A(1),rt_rf32(G_MEM,c->esp+12))));'
                if name=='pEfxAPI_R_TempSprite':body='RET(temp_guest(engine.pEfxAPI->R_TempSprite((float*)gs(A(0)),(float*)gs(A(1)),rt_rf32(G_MEM,c->esp+12),A(3),A(4),A(5),rt_rf32(G_MEM,c->esp+28),rt_rf32(G_MEM,c->esp+32),A(8))));'
                if name in ('pEfxAPI_R_Sprite_Smoke','pEfxAPI_R_Sprite_WallPuff'):body=f'engine.{e["name"]}->{match[2]}(temp_host(A(0)),rt_rf32(G_MEM,c->esp+8));RET(0);'
                if name=='pEfxAPI_R_Sprite_Explode':body='engine.pEfxAPI->R_Sprite_Explode(temp_host(A(0)),rt_rf32(G_MEM,c->esp+8),A(2));RET(0);'
                if name=='pEfxAPI_R_EntityParticles':body='engine.pEfxAPI->R_EntityParticles(entity_host(A(0)));RET(0);'
                if name=='pEfxAPI_R_FizzEffect':body='engine.pEfxAPI->R_FizzEffect(entity_host(A(0)),A(1),A(2));RET(0);'
                if name=='pEfxAPI_R_RicochetSprite':body='engine.pEfxAPI->R_RicochetSprite((float*)gs(A(0)),model_host(A(1)),rt_rf32(G_MEM,c->esp+12),rt_rf32(G_MEM,c->esp+16));RET(0);'
                # Triangle calls consume their explicit scalar/array inputs
                # and renderer state, never the mirrored client entity list.
                # Copying that list per vertex makes immediate drawing costly.
                # Audited pinned cl_game/pm_trace implementations use only
                # model/decal tables or prepared physents. The Ext trace can
                # reenter guest code and retains the full boundary.
                independent_sub = {'pEventAPI_EV_FindModelIndex',
                    'pEventAPI_EV_SetTraceHull', 'pEventAPI_EV_PlayerTrace',
                    'pEfxAPI_Draw_DecalIndex', 'pEfxAPI_Draw_DecalIndexFromName',
                    'pEfxAPI_R_GetPackedColor', 'pEfxAPI_R_LookupColor'}
                sync='' if e['name']=='pTriAPI' or name in independent_sub else 'entities_push(); '
                if body and not sync:body=body.replace('RET(','RAW_RET(')
                subcode += f'static void sub_{name}(Ctx *c) {{ '+sync+(body or f'client_missing("{e["name"]}.{match[2]}");')+' }\n'
                builders += f'if(engine.{e["name"]}->{match[2]})rt_w32(G_MEM,p+{i*4},bhl_thunk("client-subtable!{name}",sub_{name}));\n'
            elif raw == 'int version':
                builders += f'rt_w32(G_MEM,p+{i*4},engine.{e["name"]}->version);\n'
            else: raise ValueError(raw)
        builders += 'return p; }\n'
    subcode += 'static uint32_t client_subtable(const char *name,const void *host) { if(!host)return 0;\n'+builders+'client_missing(name);return 0; }\n'
    (out/'client_subtables.h').write_text(subcode)
    studio_text=strip_comments((upstream/'common/r_studioint.h').read_text())
    studio_body=studio_text.split('typedef struct engine_studio_api_s')[1].split('{',1)[1].split('} engine_studio_api_t')[0]
    studio_fields=[]
    for raw in studio_body.split(';'):
        match=re.fullmatch(r'(.+?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)',' '.join(raw.split()))
        if match:studio_fields.append(match[2])
        elif raw.strip():raise ValueError(raw)
    assert len(studio_fields)==46
    code='/* Generated studio interface, version 1. */\n'
    for i,name in enumerate(studio_fields):
        code+=f'#define BHL_STUDIO_{name} {i}\nstatic void studio_{i}(Ctx *c){{studio_dispatch(c,{i});}}\n'
    code+='static const char *studio_names[]={'+','.join(json.dumps(n) for n in studio_fields)+'};\n'
    code+='static void studio_fill(uint32_t p){\n'
    for i,name in enumerate(studio_fields):code+=f'if(studio.{name})rt_w32(G_MEM,p+{4*i},bhl_thunk("client-studio!{name}",studio_{i}));\n'
    code+='}\n'
    (out/'studio_abi.h').write_text(code)
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
            elif returns in ('char *','const char *'): statement = 'RET(borrowed_string('+call+'));'
            else: continue
            scalars.append((e['name'],f'case BHL_CL_{e["name"]}: {statement}\n'))
    # These pinned engine calls read scalar state, do math, or draw 2D pixels.
    # They neither consume nor mutate mirrored entities/effects/studio arrays.
    # Keep all callbacks with entity effects on the synchronized path.
    independent = set('''pfnSPR_Frames pfnSPR_Height pfnSPR_Width pfnSPR_Set
        pfnSPR_Draw pfnSPR_DrawHoles pfnSPR_DrawAdditive pfnSPR_EnableScissor
        pfnSPR_DisableScissor pfnFillRGBA pfnFillRGBABlend pfnDrawCharacter
        pfnDrawConsoleString pfnDrawSetTextColor pfnDrawConsoleStringLen
        pfnDrawString pfnDrawStringReverse pfnSPR_DrawGeneric
        pfnGetCvarFloat pfnGetCvarString pfnAngleVectors GetWindowCenterX
        GetWindowCenterY GetViewAngles GetMaxClients Cmd_Argc Cmd_Argv
        GetClientMaxspeed GetMousePosition IsNoClipping GetClientTime
        pfnRandomFloat pfnRandomLong Con_IsVisible pfnGetGameDirectory
        Key_LookupBinding pfnGetLevelName IsSpectateOnly pfnGetClientOldTime
        pfnGetGravity pfnSys_FloatTime pfnGetAppID PM_PointContents PM_WaterEntity'''.split())
    (out / 'client_scalar_callbacks.h').write_text(''.join(s for n, s in scalars if n not in independent))
    (out / 'client_independent_callbacks.h').write_text(''.join(s.replace('RET(', 'RAW_RET(') for n, s in scalars if n in independent))
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
