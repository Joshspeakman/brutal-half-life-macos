/* Shared guest x86 varargs formatting for native engine consoles. */
#include <ctype.h>
static void format_guest(char output[8192],const char *format,uint32_t args)
{
    size_t used=0;
    while(*format){
        if(*format!='%'){if(used==8191)abort();output[used++]=*format++;continue;}
        format++;if(*format=='%'){if(used==8191)abort();output[used++]='%';format++;continue;}
        char token[96]="%";size_t t=1;
        while(*format&&strchr("-+ #0",*format)){if(t>=70)abort();token[t++]=*format++;}
        for(int part=0;part<2;part++){
            if(part){if(*format!='.')break;token[t++]=*format++;}
            if(*format=='*'){
                int32_t width=(int32_t)rt_r32(G_MEM,args);args+=4;format++;
                if(part&&width<0){t--;continue;}
                int n=snprintf(token+t,sizeof token-t,"%d",width);if(n<0||(size_t)n>=sizeof token-t)abort();t+=n;
            }else while(isdigit((unsigned char)*format)){if(t>=70)abort();token[t++]=*format++;}
        }
        int wide=0;
        if(format[0]=='I'&&format[1]=='6'&&format[2]=='4'){wide=1;format+=3;}
        else if(*format=='l'){format++;if(*format=='l'){wide=1;format++;}}
        else if(*format=='h'){format++;if(*format=='h')format++;}
        char spec=*format++;int n=-1;
        if(strchr("diuoxX",spec)){
            if(wide){token[t++]='l';token[t++]='l';}token[t++]=spec;token[t]=0;
            if(wide){uint64_t v=rt_r64(G_MEM,args);args+=8;n=snprintf(output+used,8192-used,token,v);}
            else{uint32_t v=rt_r32(G_MEM,args);args+=4;n=snprintf(output+used,8192-used,token,v);}
        }else if(strchr("fFeEgGaA",spec)){
            token[t++]=spec;token[t]=0;double v=rt_rf64(G_MEM,args);args+=8;n=snprintf(output+used,8192-used,token,v);
        }else if(spec=='s'){
            token[t++]='s';token[t]=0;uint32_t p=rt_r32(G_MEM,args);args+=4;n=snprintf(output+used,8192-used,token,p?gs(p):"(null)");
        }else if(spec=='c'){
            token[t++]='c';token[t]=0;uint32_t v=rt_r32(G_MEM,args);args+=4;n=snprintf(output+used,8192-used,token,(int)v);
        }else if(spec=='p'){
            uint32_t v=rt_r32(G_MEM,args);args+=4;n=snprintf(output+used,8192-used,"%08X",v);
        }else{fprintf(stderr,"BHL native bridge: unsupported guest printf conversion %c\n",spec);abort();}
        if(n<0||(size_t)n>=8192-used)abort();used+=n;
    }
    output[used]=0;
}
