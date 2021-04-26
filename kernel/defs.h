#include "types.h"




// panic.c
void loop();

// sbi.c
void console_putchar(int);
int console_getchar();
void shutdown();

// console.c
void consputc(int);

// printf.c
void printf(char *, ...);
void ErrorStr(char *);
void WarnStr(char *);
void InfoStr(char *);
void InfoData(char *,char *,char *);
void DebugStr(char *);
void TraceStr(char *);
//void Info(int start,int end);
void printfinit(void);
void panic(char*);

// number of elements in fixed-size array
#define NELEM(x) (sizeof(x) / sizeof((x)[0]))


