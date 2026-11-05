// SPDX-License-Identifier: GPL-2.0
/* Freestanding bounded HTTP formatting. Only the protocol's %s/%zu/%zx/%llu
 * forms are supported; unsupported formats fail rather than touching va_args. */
#include <linux/types.h>
#include <linux/string.h>
#include <linux/stdarg.h>
#include <linux/kernel.h>
void *memmove(void *dest,const void *src,size_t n){u8 *d=dest;const u8 *s=src;if(d>s&&d-s<n)while(n--){d[n]=s[n];}else memcpy(d,s,n);return dest;}
size_t strlen(const char *s){const char *p=s;while(*p)p++;return p-s;}
int strncmp(const char *a,const char *b,size_t n){while(n--){unsigned char x=*a++,y=*b++;if(x!=y||!x)return x-y;}return 0;}
int strncasecmp(const char *a,const char *b,size_t n){while(n--){unsigned char x=*a++,y=*b++;if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y||!x)return x-y;}return 0;}
char *strstr(const char *a,const char *b){size_t n=strlen(b);for(;*a;a++)if(!strncmp(a,b,n))return (char *)a;return n?NULL:(char *)a;}
static void emit(char *out,size_t cap,size_t *at,char c){if(*at+1<cap)out[*at]=c;(*at)++;}

int vsnprintf(char *out,size_t cap,const char *format,va_list args)
{
 size_t at=0;
 while(*format){
  if(*format!='%'){emit(out,cap,&at,*format++);continue;}format++;
  bool zero=false;if(*format=='0'){zero=true;format++;}unsigned width=0;while(*format>='0'&&*format<='9')width=width*10+*format++-'0';
  if(*format=='s'){const char *s=va_arg(args,const char *);if(!s)s="(null)";while(*s)emit(out,cap,&at,*s++);format++;continue;}
  if(*format=='%'){emit(out,cap,&at,*format++);continue;}
  unsigned size=0;if(*format=='z'){size=2;format++;}else while(*format=='l'){size++;format++;}
  char spec=*format++;bool sign=spec=='d'||spec=='i';unsigned base=spec=='x'||spec=='X'||spec=='p'?16:10;u64 value;
  if(spec=='p')value=(u64)va_arg(args,void *);
  else if(sign){s64 n=size?va_arg(args,long long):va_arg(args,int);if(n<0){emit(out,cap,&at,'-');value=0-(u64)n;if(width)width--;}else value=n;}
  else if(spec=='u'||spec=='x'||spec=='X')value=size?va_arg(args,unsigned long long):va_arg(args,unsigned);
  else return -1;
  char digits[32];unsigned n=0;do{digits[n++]="0123456789abcdef"[value%base];value/=base;}while(value);
  while(width>n){emit(out,cap,&at,zero?'0':' ');width--;}while(n)emit(out,cap,&at,digits[--n]);
 }
 if(cap)out[at<cap?at:cap-1]=0;return at;
}
int snprintf(char *out,size_t cap,const char *format,...){va_list args;va_start(args,format);int n=vsnprintf(out,cap,format,args);va_end(args);return n;}
int scnprintf(char *out,size_t cap,const char *format,...){va_list args;va_start(args,format);int n=vsnprintf(out,cap,format,args);va_end(args);return n<0?n:(cap?min_t(size_t,n,cap-1):0);}
void role_printf(const char *format,...){char buffer[768];va_list args;va_start(args,format);int n=vsnprintf(buffer,sizeof(buffer),format,args);va_end(args);extern void role_puts(const char *);role_puts(n<0?"ROLE_FORMAT_ERROR\n":buffer);}
void __attribute__((noreturn)) role_panic(const char *format,...){char buffer[768];va_list args;va_start(args,format);int n=vsnprintf(buffer,sizeof(buffer),format,args);va_end(args);extern void role_puts(const char *);extern void role_runtime_fault(void) __attribute__((noreturn));role_puts(n<0?"ROLE_FORMAT_ERROR\n":buffer);role_runtime_fault();}
int strcmp(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return (unsigned char)*a-(unsigned char)*b;}
void *memchr(const void *p,int c,size_t n){const unsigned char *s=p;while(n--){if(*s==(unsigned char)c)return (void *)s;s++;}return NULL;}
char *strchr(const char *s,int c){do{if(*s==c)return (char *)s;}while(*s++);return NULL;}
