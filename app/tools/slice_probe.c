
#include <stdio.h>
#include <string.h>
#include "wv_sample.h"
#include "wv_slice.h"
int main(void){
    static unsigned char blob[300000];
    FILE *f=fopen("app/build/gen/samples.bin","rb");
    size_t n=fread(blob,1,sizeof blob,f); fclose(f);
    wv_sample s[8];
    unsigned c=wv_smp_parse(blob,(unsigned)n,s,8);
    printf("slots=%u\n",c);
    for(unsigned i=0;i<c;i++){
        wv_slice sl[16];
        unsigned k=wv_slices_split(&s[i],0.02f,200,sl,16);
        printf("%-8s len=%u rate=%u -> %u slices:", s[i].name, s[i].len, s[i].rate, k);
        for(unsigned j=0;j<k;j++) printf(" [%u..%u]", sl[j].start, sl[j].end);
        printf("\n");
    }
    return 0;
}
