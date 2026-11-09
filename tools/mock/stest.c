#include "dec.h"
#include <stdio.h>
#include <stdlib.h>
int main(int c,char**v){dec_init();Dec d;dec_open(&d,v[1]);float a=dec_seek(&d,atof(v[2]));printf("%f\n",a);FILE*o=fopen("s.raw","wb");short b[2304];for(int i=0;i<10;i++){int n=dec_read(&d,b,1152);fwrite(b,4,n,o);}fclose(o);}
