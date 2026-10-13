/* The memory-DC/font subset used by VGUI. CoreText rasterizes into guest DIBs.
 * Handles never contain host pointers. This is not a general GDI emulator. */
#include "win32.h"
#include <CoreText/CoreText.h>
#include <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#define A(n) rt_r32(G_MEM,c->esp+4+4*(n))
#define RET(v,n) do { c->eax=(uint32_t)(v);c->esp+=4+4*(n);return 1; } while(0)
extern int bhl_heap_free(uint32_t);
extern void bhl_set_last_error(uint32_t);
enum { FONT=1, BITMAP, DC };
typedef struct Gdi {
    unsigned kind,selected;uint32_t handle;
    CTFontRef font;int ascent,descent,height,weight,italic,underline,strike,charset;
    int abc[256][3];CGGlyph glyph[256];
    uint32_t pixels;int width,rows,topdown;CGContextRef context;
    uint32_t selected_font,selected_bitmap,textcolor,bgcolor,align,bgmode;
    int x,y;
    struct Gdi *next;
} Gdi;
static Gdi *objects;static uint32_t serial;
/* GDI is used on the engine/UI thread. Concurrent access remains unsupported. */
static Gdi *get(uint32_t h,unsigned kind){for(Gdi *g=objects;g;g=g->next)if(g->handle==h&&(!kind||g->kind==kind))return g;return NULL;}
static Gdi *create(unsigned kind){Gdi *g=calloc(1,sizeof *g);if(!g)abort();g->kind=kind;g->handle=0x04000000u+(++serial)*4;g->next=objects;objects=g;return g;}
static void missing(Ctx *c,const char *s){rt_unhandled(c,rt_r32(G_MEM,c->esp),s);}
static UniChar character(unsigned ch,int charset)
{
    if(charset==2)return 0xf000+ch;
    uint8_t byte=ch;UniChar result=ch;
    CFStringRef text=CFStringCreateWithBytes(NULL,&byte,1,kCFStringEncodingWindowsLatin1,false);
    if(text){if(CFStringGetLength(text))result=CFStringGetCharacterAtIndex(text,0);CFRelease(text);}return result;
}
static Gdi *new_font(const char *name,int height,int weight,int italic,int underline,int strike,int charset)
{
    Gdi *g=create(FONT);g->weight=weight?weight:400;g->italic=!!italic;g->underline=!!underline;g->strike=!!strike;g->charset=charset;
    CFStringRef face=CFStringCreateWithCString(NULL,name&&*name?name:"Helvetica",kCFStringEncodingWindowsLatin1);
    CTFontRef font=CTFontCreateWithName(face?face:CFSTR("Helvetica"),height?abs(height):16,NULL);if(face)CFRelease(face);
    CTFontSymbolicTraits traits=(weight>=600?kCTFontBoldTrait:0)|(italic?kCTFontItalicTrait:0);
    if(traits){CTFontRef variant=CTFontCreateCopyWithSymbolicTraits(font,0,NULL,traits,kCTFontBoldTrait|kCTFontItalicTrait);if(variant){CFRelease(font);font=variant;}}
    if(height>0){double cell=CTFontGetAscent(font)+CTFontGetDescent(font);CTFontRef sized=CTFontCreateCopyWithAttributes(font,CTFontGetSize(font)*height/cell,NULL,NULL);CFRelease(font);font=sized;}
    g->font=font;g->ascent=(int)ceil(CTFontGetAscent(font));g->descent=(int)ceil(CTFontGetDescent(font));g->height=g->ascent+g->descent;
    for(unsigned i=0;i<256;i++){
        UniChar ch=character(i,charset);CTFontGetGlyphsForCharacters(font,&ch,&g->glyph[i],1);
        CGSize advance;CGRect box=CTFontGetBoundingRectsForGlyphs(font,kCTFontOrientationHorizontal,&g->glyph[i],NULL,1);
        CTFontGetAdvancesForGlyphs(font,kCTFontOrientationHorizontal,&g->glyph[i],&advance,1);
        int a=(int)floor(box.origin.x),b=(int)ceil(box.origin.x+box.size.width)-a,total=(int)lround(advance.width);
        if(i<32||i==127){a=0;b=0;total=i==9?4*(int)lround(CTFontGetSize(font)/2):0;}
        g->abc[i][0]=a;g->abc[i][1]=b;g->abc[i][2]=total-a-b;
    }
    return g;
}
static void color(CGContextRef cg,uint32_t rgb){CGContextSetRGBFillColor(cg,(rgb&255)/255.,((rgb>>8)&255)/255.,((rgb>>16)&255)/255.,1);}
static int text_out(Ctx *c,Gdi *dc)
{
    Gdi *font=get(dc->selected_font,FONT),*bitmap=get(dc->selected_bitmap,BITMAP);
    if(!font||!bitmap||!bitmap->context){bhl_set_last_error(6);return 0;}
    unsigned flags=A(3),count=A(6);if(count>8192||flags&~6u||dc->align&~1u){missing(c,"GDI text flags/alignment outside VGUI subset");return 0;}
    int x=dc->align&1?dc->x:(int)A(1),y=dc->align&1?dc->y:(int)A(2),width=0;
    for(unsigned i=0;i<count;i++){unsigned ch=G_MEM[A(5)+i];width+=A(7)?(int)rt_r32(G_MEM,A(7)+4*i):font->abc[ch][0]+font->abc[ch][1]+font->abc[ch][2];}
    CGContextRef cg=bitmap->context;CGContextSaveGState(cg);
    /* Quartz rows increase upward; a negative DIB height means row zero is top. */
    if(bitmap->topdown){CGContextTranslateCTM(cg,0,bitmap->rows);CGContextScaleCTM(cg,1,-1);}
    CGRect rect=CGRectMake(x,y,width,font->height);
    if(flags&&A(4)){int32_t r[4];memcpy(r,G_MEM+A(4),16);rect=CGRectMake(r[0],r[1],r[2]-r[0],r[3]-r[1]);}
    if(flags&4)CGContextClipToRect(cg,rect);
    if(dc->bgmode==2||(flags&2)){color(cg,dc->bgcolor);CGContextFillRect(cg,rect);}
    color(cg,dc->textcolor);CGContextSetShouldAntialias(cg,true);CGContextSetShouldSmoothFonts(cg,false);
    CGContextSetTextMatrix(cg,CGAffineTransformIdentity);
    int pen=x;
    for(unsigned i=0;i<count;i++){
        unsigned ch=G_MEM[A(5)+i];CGPoint point=CGPointZero;
        if(ch>=32&&ch!=127){CGContextSaveGState(cg);CGContextTranslateCTM(cg,pen,y+font->ascent);CGContextScaleCTM(cg,1,-1);CTFontDrawGlyphs(font->font,&font->glyph[ch],&point,1,cg);CGContextRestoreGState(cg);}
        pen+=A(7)?(int)rt_r32(G_MEM,A(7)+4*i):font->abc[ch][0]+font->abc[ch][1]+font->abc[ch][2];
    }
    if(font->underline)CGContextFillRect(cg,CGRectMake(x,y+font->ascent+1,width,fmax(1,CTFontGetUnderlineThickness(font->font))));
    if(font->strike)CGContextFillRect(cg,CGRectMake(x,y+font->ascent/2,width,1));
    CGContextRestoreGState(cg);CGContextFlush(cg);if(dc->align&1)dc->x=pen;return 1;
}
int bhl_gdi_import(Ctx *c,const char *qualified)
{
    if(strncasecmp(qualified,"GDI32.dll!",10))return 0;const char *name=qualified+10;
    if(!strcmp(name,"CreateFontA")){
        if((int)A(0)<-2048||(int)A(0)>2048||A(1)||A(2)||A(3)||A(8)>2||A(9)||A(10)||A(11)||A(12)){missing(c,"GDI font geometry/charset/precision outside VGUI subset");return 1;}
        Gdi *g=new_font(A(13)?(char*)G_MEM+A(13):NULL,A(0),A(4),A(5),A(6),A(7),A(8));RET(g->handle,14);
    }
    if(!strcmp(name,"CreateCompatibleDC")){
        if(A(0)&&!get(A(0),DC)){bhl_set_last_error(6);RET(0,1);}
        Gdi *dc=create(DC);dc->bgcolor=0xffffff;dc->bgmode=2;RET(dc->handle,1);
    }
    if(!strcmp(name,"SelectObject")){
        Gdi *dc=get(A(0),DC),*obj=get(A(1),0);if(!dc||!obj||(obj->kind!=FONT&&obj->kind!=BITMAP)){bhl_set_last_error(6);RET(0,2);}
        uint32_t *slot=obj->kind==FONT?&dc->selected_font:&dc->selected_bitmap;
        if(obj->kind==BITMAP&&obj->selected&&*slot!=obj->handle)RET(0,2);
        uint32_t old=*slot;Gdi *previous=get(old,0);if(previous)previous->selected--;obj->selected++;*slot=obj->handle;RET(old,2);
    }
    if(!strcmp(name,"CreateDIBSection")){
        uint32_t p=A(1);int32_t width=rt_r32(G_MEM,p+4),height=rt_r32(G_MEM,p+8);
        if(rt_r32(G_MEM,p)!=40||width<=0||width>8192||!height||height>8192||height<-8192||rt_r16(G_MEM,p+12)!=1||rt_r16(G_MEM,p+14)!=32||rt_r32(G_MEM,p+16)||A(2)||A(4)||A(5)||!A(3)){missing(c,"GDI DIB format outside 32-bit uncompressed RGB");return 1;}
        Gdi *g=create(BITMAP);g->width=width;g->rows=abs(height);g->topdown=height<0;g->pixels=bhl_heap_alloc(width*g->rows*4,1);if(!g->pixels)abort();
        CGColorSpaceRef rgb=CGColorSpaceCreateDeviceRGB();g->context=CGBitmapContextCreate(G_MEM+g->pixels,width,g->rows,8,width*4,rgb,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst);CGColorSpaceRelease(rgb);if(!g->context)abort();
        rt_w32(G_MEM,A(3),g->pixels);RET(g->handle,6);
    }
    if(!strcmp(name,"DeleteObject")||!strcmp(name,"DeleteDC")){
        Gdi *g=get(A(0),0);if(!g||g->selected||(!strcmp(name,"DeleteDC")?g->kind!=DC:g->kind==DC))RET(0,1);
        if(g->font)CFRelease(g->font);if(g->context)CGContextRelease(g->context);if(g->pixels)bhl_heap_free(g->pixels);
        Gdi *f=get(g->selected_font,FONT),*b=get(g->selected_bitmap,BITMAP);if(f)f->selected--;if(b)b->selected--;
        Gdi **link=&objects;while(*link!=g)link=&(*link)->next;*link=g->next;free(g);RET(1,1);
    }
    Gdi *dc=get(A(0),DC);if(!dc){bhl_set_last_error(6);return 0;}
    if(!strcmp(name,"SetTextAlign")){if(A(1)&~1u){missing(c,"GDI text alignment outside VGUI subset");return 1;}uint32_t old=dc->align;dc->align=A(1);RET(old,2);}
    if(!strcmp(name,"SetBkMode")){if(A(1)!=1&&A(1)!=2)RET(0,2);uint32_t old=dc->bgmode;dc->bgmode=A(1);RET(old,2);}
    if(!strcmp(name,"SetTextColor")||!strcmp(name,"SetBkColor")){uint32_t *slot=!strcmp(name,"SetTextColor")?&dc->textcolor:&dc->bgcolor,old=*slot;*slot=A(1);RET(old,2);}
    if(!strcmp(name,"MoveToEx")){if(A(3)){rt_w32(G_MEM,A(3),dc->x);rt_w32(G_MEM,A(3)+4,dc->y);}dc->x=A(1);dc->y=A(2);RET(1,4);}
    if(!strcmp(name,"GetTextMetricsA")){
        Gdi *f=get(dc->selected_font,FONT);if(!f||!A(1))RET(0,2);uint32_t p=A(1);memset(G_MEM+p,0,56);int max=1,sum=0;
        for(int i=32;i<256;i++){int w=f->abc[i][0]+f->abc[i][1]+f->abc[i][2];sum+=w;if(w>max)max=w;if(f->abc[i][1]>max)max=f->abc[i][1];}
        int values[]={f->height,f->ascent,f->descent,(int)fmax(0,f->height-CTFontGetSize(f->font)),(int)ceil(CTFontGetLeading(f->font)),sum/224,max,f->weight,0,96,96};
        memcpy(G_MEM+p,values,sizeof values);G_MEM[p+44]=0;G_MEM[p+45]=255;G_MEM[p+46]='?';G_MEM[p+47]=' ';G_MEM[p+48]=f->italic;G_MEM[p+49]=f->underline;G_MEM[p+50]=f->strike;G_MEM[p+51]=6;G_MEM[p+52]=f->charset;RET(1,2);
    }
    if(!strcmp(name,"GetCharABCWidthsA")){Gdi *f=get(dc->selected_font,FONT);if(!f||A(1)>A(2)||A(2)>255||!A(3))RET(0,4);memcpy(G_MEM+A(3),f->abc[A(1)],12*(A(2)-A(1)+1));RET(1,4);}
    if(!strcmp(name,"ExtTextOutA")){int result=text_out(c,dc);RET(result,8);}
    return 0;
}
