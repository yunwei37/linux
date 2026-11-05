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
int snprintf(char *out,size_t cap,const char *format,...)
{
 va_list args;size_t at=0;va_start(args,format);
 while(*format){
  if(*format!='%'){emit(out,cap,&at,*format++);continue;}
  format++;
  if(*format=='s'){const char *s=va_arg(args,const char *);while(*s)emit(out,cap,&at,*s++);format++;continue;}
  if(*format=='%'){emit(out,cap,&at,*format++);continue;}
  u64 value;unsigned base=10;
  if(*format=='z'){value=va_arg(args,size_t);format++;}
  else if(format[0]=='l'&&format[1]=='l'){value=va_arg(args,unsigned long long);format+=2;}
  else{va_end(args);return -1;}
  if(*format=='x')base=16;else if(*format!='u'){va_end(args);return -1;}format++;
  char digits[32];unsigned n=0;do{digits[n++]="0123456789abcdef"[value%base];value/=base;}while(value);
  while(n)emit(out,cap,&at,digits[--n]);
 }
 if(cap)out[at<cap?at:cap-1]=0;va_end(args);return at;
}
