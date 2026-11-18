#include "ternfold/core_lower.h"
#include "../src/typed_signatures.h"
#include "../src/backend/driver_native.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define MKDIR(path) _mkdir(path)
#define RMDIR(path) _rmdir(path)
#define PID() ((unsigned long)_getpid())
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(path) mkdir(path,0700)
#define RMDIR(path) rmdir(path)
#define PID() ((unsigned long)getpid())
#endif

#define CHECK(ok) do { if (!(ok)) { fprintf(stderr,"test_driver_native:%d: %s\n",__LINE__,#ok); exit(1); } } while (0)
static const char root_source[]=
    "import \"library.tfd\" as helpers;\n"
    "pub fn main() -> List[Text] { [helpers::line()] }\n";
static const char library_source[]="pub fn line() -> Text { \"retained source\" }\n";
static const char replacement[]="// new file at the cached canonical path\n";

static void path(char *out,size_t cap,const char *dir,const char *leaf) {
    const int n=snprintf(out,cap,"%s/%s",dir,leaf);
    CHECK(n>0 && (size_t)n<cap);
}
static void write_source(const char *file,const char *bytes) {
    FILE *out=fopen(file,"wbx"); CHECK(out!=NULL);
    CHECK(fwrite(bytes,1,strlen(bytes),out)==strlen(bytes) && fclose(out)==0);
}
static void unchanged(const char *file,const char *bytes) {
    FILE *in=fopen(file,"rb"); CHECK(in!=NULL);
    for (size_t i=0;bytes[i]!='\0';++i) CHECK(fgetc(in)==(unsigned char)bytes[i]);
    CHECK(fgetc(in)==EOF && !ferror(in) && fclose(in)==0);
}

int main(void) {
    char directory[128],root_path[192],library_path[192],saved_path[192];
    bool created=false;
    for (unsigned int i=0;i<64;++i) {
        const int n=snprintf(directory,sizeof directory,"build/driver-native-snapshots-%lu-%u",PID(),i);
        CHECK(n>0 && (size_t)n<sizeof directory);
        if (MKDIR(directory)==0) { created=true; break; }
    }
    CHECK(created);
    path(root_path,sizeof root_path,directory,"root.tfd");
    path(library_path,sizeof library_path,directory,"library.tfd");
    path(saved_path,sizeof saved_path,directory,"saved-original.tfd");
    write_source(root_path,root_source); write_source(library_path,library_source);
    TfSourceManager *manager=NULL; TfResolution *resolution=NULL;
    TfTypedSignatures *signatures=NULL; TfTypedProgram *typed=NULL;
    TfMachineProgram *machines=NULL; TfCoreProgram *core=NULL; TfVerifiedCore *verified=NULL;
    const TfResolveOptions options={directory,100};
    CHECK(tf_source_manager_create(&manager,NULL)==TF_OK);
    CHECK(tf_resolve(&resolution,manager,root_path,&options,NULL)==TF_OK);
    CHECK(!tf_diagnostic_bag_has_errors(tf_resolution_diagnostics(resolution)));
    CHECK(tf_signatures_build(&signatures,resolution,100,NULL)==TF_OK);
    CHECK(tf_check(&typed,resolution,signatures,100,NULL)==TF_OK);
    CHECK(tf_machine_program_build(&machines,typed,100,NULL)==TF_OK);
    CHECK(tf_core_lower(&core,machines,NULL,NULL)==TF_OK);
    CHECK(tf_core_verify(&verified,core,NULL,NULL)==TF_OK);
    const char *const sources[]={root_path,library_path};
    const char *const contents[]={root_source,library_source};
    for (size_t i=0;i<2;++i) {
        const TfManagedSource *snapshot=NULL,*cached=NULL;
        CHECK(tf_source_manager_load(manager,sources[i],&snapshot)==TF_OK && snapshot->source->origin.valid);
        CHECK(rename(sources[i],saved_path)==0);
        write_source(sources[i],replacement);
        CHECK(tf_source_manager_load(manager,sources[i],&cached)==TF_OK && cached==snapshot);
        for (size_t build=0;build<2;++build) {
            const TfDriverNativeOptions native={.output=saved_path,.compiler="unused-compiler"};
            const char *failure=tf_driver_native(verified,resolution,&native,build!=0);
            CHECK(failure!=NULL && strstr(failure,"aliases a Ternfold source")!=NULL);
            unchanged(saved_path,contents[i]); unchanged(sources[i],replacement);
        }
        CHECK(remove(sources[i])==0 && rename(saved_path,sources[i])==0);
    }
    tf_verified_core_destroy(verified); tf_core_program_destroy(core);
    tf_machine_program_destroy(machines); tf_typed_program_destroy(typed);
    tf_signatures_destroy(signatures); tf_resolution_destroy(resolution); tf_source_manager_destroy(manager);
    CHECK(remove(root_path)==0 && remove(library_path)==0 && RMDIR(directory)==0);
    puts("Native driver: root/import retained identities protected for emit-c and build");
    return 0;
}
