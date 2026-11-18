#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void present(const char *path) {
    FILE *file=fopen(path,"rb");
    if (file==NULL) { fprintf(stderr,"distribution: missing %s\n",path); ++failures; }
    else if (fclose(file)!=0) ++failures;
}

static void scanner(void) {
    FILE *file=fopen("src/lexer.c","rb");
    if (file==NULL) { ++failures; return; }
    char *bytes=malloc(1048577);
    if (bytes==NULL) { fclose(file); ++failures; return; }
    const size_t length=fread(bytes,1,1048577,file);
    const int read_error=ferror(file);
    const int closed=fclose(file);
    if (length>1048576 || read_error || closed!=0) ++failures;
    else {
        bytes[length]=0;
        if (strstr(bytes,"strcmp") || strstr(bytes,"strncmp") || strstr(bytes,"strstr")) {
            fputs("distribution: scanner must use character transitions\n",stderr);
            ++failures;
        }
    }
    free(bytes);
}

int main(void) {
    const char *const required[]={
        "README.md","CONTRIBUTING.md","docs/README.md","docs/language-spec.md",
        "docs/grammar.md","docs/core-ir.md","docs/runtime.md","docs/releases.md",
        "examples/manifest.json","stdlib/ints.tfd","stdlib/text.tfd",
        "src/compiler_main.c","src/backend/c_emit.c","src/execution/interpreter.c",
        "benchmarks/three-way.ps1","experiments/composition/run.ps1"
    };
    for (size_t i=0;i<sizeof required/sizeof *required;++i) present(required[i]);
    scanner();
    if (failures==0) puts("Distribution audit: required sources and scanner rules passed");
    return failures==0 ? 0 : 1;
}
