/* Isolated GPU readback checks. Interpose the client resolver registration, then
 * draw into the hidden test window at its next swap. Never inject into user play.
 * The fixture exits after checking pixels; full lifecycle is tested separately. */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <OpenGL/gl.h>
static void *(*resolve)(const char *);
extern void bhl_set_graphics_proc(void *(*)(const char *));
static void register_proc(void *(*p)(const char *))
{ resolve=p; bhl_set_graphics_proc(p); }
#define FN(result,name,args) static result (*q##name) args
FN(void,Enable,(unsigned)); FN(void,Disable,(unsigned));
FN(void,Begin,(unsigned)); FN(void,End,(void));
FN(void,Color4f,(float,float,float,float)); FN(void,Vertex3f,(float,float,float));
FN(void,TexCoord2f,(float,float)); FN(void,MultiTexCoord2f,(unsigned,float,float));
FN(void,MatrixMode,(unsigned)); FN(void,LoadIdentity,(void));
FN(void,Ortho,(double,double,double,double,double,double));
FN(void,Translatef,(float,float,float)); FN(void,Scalef,(float,float,float));
FN(void,Fogi,(unsigned,int)); FN(void,Fogf,(unsigned,float)); FN(void,Fogfv,(unsigned,const float *));
FN(void,ActiveTextureARB,(unsigned)); FN(void,BindTexture,(unsigned,unsigned));
FN(void,GenTextures,(int,unsigned *)); FN(void,TexParameteri,(unsigned,unsigned,int));
FN(void,TexImage2D,(unsigned,int,int,int,int,int,unsigned,unsigned,const void *));
FN(void,TexEnvi,(unsigned,unsigned,int)); FN(void,AlphaFunc,(unsigned,float));
FN(void,Viewport,(int,int,int,int)); FN(void,ClearColor,(float,float,float,float)); FN(void,Clear,(unsigned));
FN(void,ReadPixels,(int,int,int,int,unsigned,unsigned,void *));
FN(unsigned,GetError,(void)); FN(void,BlendFunc,(unsigned,unsigned));
FN(void,DepthFunc,(unsigned)); FN(void,DepthMask,(unsigned char));
static FILE *output;
static unsigned tests,failures;
static void load_functions(void)
{
#define LOAD(name) q##name=resolve("gl"#name); if(!q##name){fprintf(stderr,"Render contract: missing gl%s\n",#name);_Exit(2);}
    LOAD(Enable);LOAD(Disable);LOAD(Begin);LOAD(End);LOAD(Color4f);LOAD(Vertex3f);
    LOAD(TexCoord2f);LOAD(MultiTexCoord2f);LOAD(MatrixMode);LOAD(LoadIdentity);LOAD(Ortho);
    LOAD(Translatef);LOAD(Scalef);LOAD(Fogi);LOAD(Fogf);LOAD(Fogfv);LOAD(ActiveTextureARB);
    LOAD(BindTexture);LOAD(GenTextures);LOAD(TexParameteri);LOAD(TexImage2D);LOAD(TexEnvi);
    LOAD(AlphaFunc);LOAD(Viewport);LOAD(ClearColor);LOAD(Clear);LOAD(ReadPixels);LOAD(GetError);
    LOAD(BlendFunc);LOAD(DepthFunc);LOAD(DepthMask);
#undef LOAD
}
static void clear(void) { qClearColor(0,0,0,1);qClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); }
static void quad(float z,int textured)
{
    qBegin(GL_QUADS);
    const float vertices[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
    for(unsigned i=0;i<4;i++){
        if(textured)qTexCoord2f(.25f,.5f);
        if(textured==2)qMultiTexCoord2f(GL_TEXTURE1_ARB,.5f,.5f);
        qVertex3f(vertices[i][0],vertices[i][1],z);
    }
    qEnd();
}
static void check(const char *name,int r,int g,int b)
{
    unsigned char pixel[4];qReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    unsigned error=qGetError();int ok=!error&&abs(pixel[0]-r)<=2&&abs(pixel[1]-g)<=2&&abs(pixel[2]-b)<=2;
    fprintf(output,"%s{\"name\":\"%s\",\"rgb\":[%u,%u,%u],\"expected\":[%d,%d,%d],\"gl_error\":%u,\"passed\":%s}",
            tests++?",\n":"",name,pixel[0],pixel[1],pixel[2],r,g,b,error,ok?"true":"false");
    failures+=!ok;
}
static unsigned texture(int internal,int width,const unsigned char *pixels)
{
    unsigned t;qGenTextures(1,&t);qBindTexture(GL_TEXTURE_2D,t);
    qTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    qTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    qTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    qTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    qTexImage2D(GL_TEXTURE_2D,0,internal,width,1,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    return t;
}
static void run_contracts(void)
{
    load_functions();output=fopen(getenv("BHL_RENDER_CONTRACTS"),"w");if(!output)_Exit(2);
    fprintf(output,"{\"tests\":[\n");
    /* The engine may have issued unrelated compatibility queries before this
       fixture starts. Check every operation below against a clean error flag. */
    while(qGetError()){}
    qViewport(0,0,16,16);qDisable(GL_SCISSOR_TEST);qDisable(GL_BLEND);qDisable(GL_CULL_FACE);
    qDisable(GL_DEPTH_TEST);qDepthMask(GL_TRUE);qDisable(GL_ALPHA_TEST);qDisable(GL_FOG);
    for(int unit=1;unit>=0;unit--){
        qActiveTextureARB(GL_TEXTURE0_ARB+unit);qDisable(GL_TEXTURE_2D);
        qMatrixMode(GL_TEXTURE);qLoadIdentity();qTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
    }
    qMatrixMode(GL_MODELVIEW);qLoadIdentity();qMatrixMode(GL_PROJECTION);qLoadIdentity();
    qOrtho(-1,1,-1,1,0,200);qMatrixMode(GL_MODELVIEW);
    clear();qColor4f(1,0,0,1);quad(-50,0);check("solid",255,0,0);
    /* Particle/tracer batches take the indexed path once they exceed one quad. */
    clear();qBegin(GL_QUADS);
    for(int n=0;n<2;n++){
        qColor4f(n?0:1,n?1:0,0,1);
        float x=n?3:0;
        qVertex3f(x-1,-1,-50);qVertex3f(x+1,-1,-50);
        qVertex3f(x+1,1,-50);qVertex3f(x-1,1,-50);
    }
    qEnd();check("particle_quad_batch",255,0,0);
    float fog[]={1,0,0,1};qEnable(GL_FOG);qFogfv(GL_FOG_COLOR,fog);
    qFogf(GL_FOG_START,0);qFogf(GL_FOG_END,200);qFogf(GL_FOG_DENSITY,.01f);
    qColor4f(0,0,1,1);
    const unsigned modes[]={GL_LINEAR,GL_EXP,GL_EXP2};const char *names[]={"fog_linear","fog_exp","fog_exp2"};
    const double visibility[]={.75,0.60653066,0.77880078};
    for(int i=0;i<3;i++){clear();qFogi(GL_FOG_MODE,modes[i]);quad(-50,0);check(names[i],lround(255*(1-visibility[i])),0,lround(255*visibility[i]));}
    qDisable(GL_FOG);qEnable(GL_TEXTURE_2D);
    const unsigned char rgba[]={128,64,32,128};unsigned tex=texture(GL_RGBA,1,rgba);
    const unsigned envs[]={GL_MODULATE,GL_REPLACE,GL_DECAL,GL_ADD};
    const char *envnames[]={"modulate","replace","decal","add"};
    const int expected[][3]={{32,32,24},{128,64,32},{96,96,111},{192,192,223}};
    for(int i=0;i<4;i++){clear();qTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,envs[i]);qColor4f(.25,.5,.75,.6);quad(-50,1);check(envnames[i],expected[i][0],expected[i][1],expected[i][2]);}
    /* An RGB texture ignores source alpha during DECAL. */
    texture(GL_RGB,1,rgba);qTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_DECAL);
    clear();quad(-50,1);check("rgb_decal",128,64,32);
    const unsigned char stripe[]={255,0,0,255,0,0,255,255};texture(GL_RGBA,2,stripe);
    qTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_REPLACE);qMatrixMode(GL_TEXTURE);qTranslatef(.5,0,0);
    clear();quad(-50,1);check("texture_matrix",0,0,255);qLoadIdentity();qMatrixMode(GL_MODELVIEW);
    qBindTexture(GL_TEXTURE_2D,tex);qTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
    qActiveTextureARB(GL_TEXTURE1_ARB);qEnable(GL_TEXTURE_2D);
    const unsigned char lightmap[]={128,128,128,255};texture(GL_RGBA,1,lightmap);
    qTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);qActiveTextureARB(GL_TEXTURE0_ARB);
    clear();qColor4f(1,1,1,1);quad(-50,2);check("lightmap",64,32,16);
    qActiveTextureARB(GL_TEXTURE1_ARB);qDisable(GL_TEXTURE_2D);qActiveTextureARB(GL_TEXTURE0_ARB);qDisable(GL_TEXTURE_2D);
    qEnable(GL_ALPHA_TEST);qAlphaFunc(GL_GREATER,.5);
    clear();qColor4f(1,0,0,.25);quad(-50,0);check("alpha_discard",0,0,0);
    qColor4f(1,0,0,.75);quad(-50,0);check("alpha_visible",255,0,0);qDisable(GL_ALPHA_TEST);
    clear();qEnable(GL_BLEND);qBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    qColor4f(1,0,0,.5);quad(-50,0);check("translucent",128,0,0);
    qBlendFunc(GL_ONE,GL_ONE);qColor4f(0,.5,0,1);quad(-50,0);check("additive",128,128,0);qDisable(GL_BLEND);
    clear();qEnable(GL_DEPTH_TEST);qDepthFunc(GL_LEQUAL);
    qColor4f(1,0,0,1);quad(-20,0);qColor4f(0,1,0,1);quad(-100,0);check("depth_order",255,0,0);
    fprintf(output,"\n],\"passed\":%s}\n",failures?"false":"true");fclose(output);
    _Exit(failures?1:0);
}
static void swap(void *window)
{if(resolve&&getenv("BHL_RENDER_CONTRACTS"))run_contracts();SDL_GL_SwapWindow(window);}
__attribute__((used,section("__DATA,__interpose"))) static struct {void *replacement,*original;} hooks[]={
    {register_proc,bhl_set_graphics_proc},{swap,SDL_GL_SwapWindow}
};
