/* Independent decoded schema/round-trip assertions for diagnostic JSON v2. */
#include "ternfold/diagnostics.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static size_t checks;
static void check(bool okay, int line) {
    ++checks;
    if (!okay) { fprintf(stderr,"diagnostic JSON:%d\n",line); exit(1); }
}
#define CHECK(x) check((x),__LINE__)
enum { NODES = 4096, STRINGS = 131072 };
typedef struct { char type; size_t next, offset, length; uint64_t number; } Node;
typedef struct {
    const unsigned char *input; size_t length, at, count, used;
    Node nodes[NODES]; unsigned char strings[STRINGS];
} Json;
static void ws(Json *j) {
    while (j->at < j->length && strchr(" \t\r\n",j->input[j->at]) != NULL) ++j->at;
}
static unsigned hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10u;
    if (c >= 'A' && c <= 'F') return c-'A'+10u;
    CHECK(false); return 0;
}
static uint32_t unit(Json *j) {
    uint32_t out = 0;
    CHECK(j->length-j->at >= 4);
    for (unsigned i=0;i<4;++i) out=(out<<4)|hex(j->input[j->at++]);
    return out;
}
static void append(Json *j, unsigned char c) {
    CHECK(j->used < STRINGS); j->strings[j->used++]=c;
}
static void scalar(Json *j, uint32_t c) {
    CHECK(c <= 0x10ffffu && !(c >= 0xd800u && c <= 0xdfffu));
    if(c < 0x80u) append(j,(unsigned char)c);
    else if(c < 0x800u) {append(j,(unsigned char)(0xc0u|(c>>6)));append(j,(unsigned char)(0x80u|(c&63u)));}
    else if(c < 0x10000u) {append(j,(unsigned char)(0xe0u|(c>>12)));append(j,(unsigned char)(0x80u|((c>>6)&63u)));append(j,(unsigned char)(0x80u|(c&63u)));}
    else {append(j,(unsigned char)(0xf0u|(c>>18)));append(j,(unsigned char)(0x80u|((c>>12)&63u)));append(j,(unsigned char)(0x80u|((c>>6)&63u)));append(j,(unsigned char)(0x80u|(c&63u)));}
}
static size_t parse(Json *j, unsigned depth) {
    size_t id; unsigned char c;
    CHECK(depth < 32 && j->count < NODES); ws(j); CHECK(j->at < j->length);
    id=j->count++; c=j->input[j->at++]; j->nodes[id].type=(char)c;
    if(c=='"') {
        j->nodes[id].offset=j->used;
        for (;;) {
            CHECK(j->at < j->length); c=j->input[j->at++];
            if(c=='"') break;
            CHECK(c>=32);
            if(c=='\\') {
                CHECK(j->at < j->length); c=j->input[j->at++];
                if(c=='u') {
                    uint32_t cp=unit(j);
                    if(cp>=0xd800u && cp<=0xdbffu) {
                        uint32_t low;
                        CHECK(j->length-j->at>=2 && j->input[j->at]=='\\' && j->input[j->at+1]=='u');j->at+=2;
                        low=unit(j);CHECK(low>=0xdc00u&&low<=0xdfffu);
                        cp=0x10000u+((cp-0xd800u)<<10)+(low-0xdc00u);
                    }
                    scalar(j,cp); continue;
                }
                switch(c) {
                    case '"':case '\\':case '/':break;
                    case 'b':c='\b';break; case 'f':c='\f';break;
                    case 'n':c='\n';break; case 'r':c='\r';break;case 't':c='\t';break;
                    default:CHECK(false);
                }
            }
            append(j,c);
        }
        j->nodes[id].length=j->used-j->nodes[id].offset;
    } else if(c=='{' || c=='[') {
        unsigned char close=c=='{'?'}':']';ws(j);CHECK(j->at<j->length);
        if(j->input[j->at]!=close) for(;;) {
            if(c=='{') {size_t key=parse(j,depth+1);CHECK(j->nodes[key].type=='"');ws(j);CHECK(j->at<j->length&&j->input[j->at++]==':');}
            (void)parse(j,depth+1);ws(j);CHECK(j->at<j->length);
            if(j->input[j->at]==close) break;
            CHECK(j->input[j->at++]==',');
        }
        ++j->at;
    } else if(c=='t'||c=='f'||c=='n') {
        const char *tail=c=='t'?"rue":c=='f'?"alse":"ull";size_t n=strlen(tail);
        CHECK(n<=j->length-j->at&&!memcmp(j->input+j->at,tail,n));j->at+=n;
    } else {
        CHECK(c>='0'&&c<='9');j->nodes[id].type='0';j->nodes[id].number=c-'0';
        while(j->at<j->length&&j->input[j->at]>='0'&&j->input[j->at]<='9') {
            unsigned digit=j->input[j->at++]-'0';
            CHECK(j->nodes[id].number<=(UINT64_MAX-digit)/10u);
            j->nodes[id].number=j->nodes[id].number*10u+digit;
        }
    }
    j->nodes[id].next=j->count;return id;
}
static void text(const Json *j,size_t id,const void *data,size_t length) {
    CHECK(id<j->count&&j->nodes[id].type=='"'&&j->nodes[id].length==length);
    CHECK(length==0||!memcmp(j->strings+j->nodes[id].offset,data,length));
}
static size_t field(const Json *j,size_t object,const char *key) {
    size_t found=j->count,n=strlen(key);
    CHECK(object<j->count&&j->nodes[object].type=='{');
    for(size_t i=object+1;i<j->nodes[object].next;) {
        CHECK(j->nodes[i].type=='"');
        if(j->nodes[i].length==n&&!memcmp(j->strings+j->nodes[i].offset,key,n)) {CHECK(found==j->count);found=i+1;}
        i=j->nodes[i+1].next;
    }
    CHECK(found<j->count);return found;
}
static void members(const Json *j,size_t node,char type,size_t count) {
    size_t actual=0;
    CHECK(j->nodes[node].type==type);
    for(size_t i=node+1;i<j->nodes[node].next;i=j->nodes[i].next) ++actual;
    CHECK(actual==(type=='{'?count*2:count));
}
static void number(const Json *j,size_t id,size_t expected) {
    CHECK(j->nodes[id].type=='0'&&j->nodes[id].number==(uint64_t)expected);
}
static void location(const Json *j,size_t id,const TfSource *source,size_t byte) {
    TfLocation l;
    if(!source->valid) {CHECK(j->nodes[id].type=='n');return;}
    CHECK(tf_source_location(source,byte,&l));members(j,id,'{',3);
    number(j,field(j,id,"line"),l.line);number(j,field(j,id,"column"),l.column);
    number(j,field(j,id,"display_column"),l.display_column);
}
static void span(const Json *j,size_t id,TfSpan expected) {
    const TfSource *s=expected.source;
    members(j,id,'{',7);text(j,field(j,id,"source"),s->name,strlen(s->name));
    number(j,field(j,id,"start_byte"),expected.start);number(j,field(j,id,"end_byte"),expected.end);
    location(j,field(j,id,"start"),s,expected.start);location(j,field(j,id,"end"),s,expected.end);
    CHECK(j->nodes[field(j,id,"raw")].type==(s->valid?'f':'t'));
    if(s->valid) text(j,field(j,id,"source_text"),s->bytes+expected.start,expected.end-expected.start);
    else {
        unsigned char encoded[512];size_t at=0;
        CHECK(expected.end-expected.start<=sizeof(encoded)/2);
        for(size_t i=expected.start;i<expected.end;++i) {
            unsigned char c=s->bytes[i];
            if(c<128) encoded[at++]=c;
            else {encoded[at++]=(unsigned char)(0xc0u|(c>>6));encoded[at++]=(unsigned char)(0x80u|(c&63u));}
        }
        text(j,field(j,id,"source_text"),encoded,at);
    }
}
static void texts(const Json *j,size_t node,const TfDiagnosticText *expected,size_t count) {
    size_t at=node+1;members(j,node,'[',count);
    for(size_t i=0;i<count;++i) {text(j,at,expected[i].data,expected[i].length);at=j->nodes[at].next;}
}
static void schema(const Json *j,const TfDiagnosticBag *bag) {
    size_t records=field(j,0,"diagnostics"),at=records+1;
    members(j,0,'{',4);number(j,field(j,0,"schema_version"),2);
    number(j,field(j,0,"suppressed"),tf_diagnostic_bag_suppressed(bag));
    CHECK(j->nodes[field(j,0,"has_errors")].type==(tf_diagnostic_bag_has_errors(bag)?'t':'f'));
    members(j,records,'[',tf_diagnostic_bag_count(bag));
    for(size_t i=0;i<tf_diagnostic_bag_count(bag);++i) {
        const TfDiagnosticRecord *r=tf_diagnostic_bag_get(bag,i);
        size_t labels=field(j,at,"labels"),edits=field(j,at,"edits"),label=labels+1,edit=edits+1;
        const char *severity=r->severity==TF_DIAG_ERROR?"error":r->severity==TF_DIAG_WARNING?"warning":"note";
        members(j,at,'{',7);text(j,field(j,at,"code"),r->code,4);text(j,field(j,at,"severity"),severity,strlen(severity));
        text(j,field(j,at,"message"),r->message.data,r->message.length);
        texts(j,field(j,at,"notes"),r->notes,r->note_count);texts(j,field(j,at,"help"),r->help,r->help_count);
        members(j,labels,'[',r->label_count);members(j,edits,'[',r->edit_count);
        for(size_t k=0;k<r->label_count;++k) {
            members(j,label,'{',3);CHECK(j->nodes[field(j,label,"primary")].type==(r->labels[k].primary?'t':'f'));
            text(j,field(j,label,"message"),r->labels[k].message.data,r->labels[k].message.length);
            span(j,field(j,label,"span"),r->labels[k].span);label=j->nodes[label].next;
        }
        for(size_t k=0;k<r->edit_count;++k) {
            members(j,edit,'{',2);text(j,field(j,edit,"replacement"),r->edits[k].replacement.data,r->edits[k].replacement.length);
            span(j,field(j,edit,"span"),r->edits[k].span);edit=j->nodes[edit].next;
        }
        at=j->nodes[at].next;
    }
}
static void roundtrip(const TfDiagnosticBag *bag) {
    FILE *f=tmpfile();long length;unsigned char *bytes;Json *j=calloc(1,sizeof(*j));
    CHECK(f!=NULL&&j!=NULL);CHECK(tf_diagnostic_bag_write(f,bag,TF_DIAG_JSON,TF_DIAG_COLOR_ALWAYS)==TF_OK);
    CHECK(fflush(f)==0&&fseek(f,0,SEEK_END)==0);length=ftell(f);CHECK(length>=0);
    bytes=malloc((size_t)length+1);CHECK(bytes!=NULL&&fseek(f,0,SEEK_SET)==0);
    CHECK(fread(bytes,1,(size_t)length,f)==(size_t)length&&fclose(f)==0);
    bytes[length]=0;CHECK(memchr(bytes,27,(size_t)length)==NULL);
    j->input=bytes;j->length=(size_t)length;(void)parse(j,0);ws(j);CHECK(j->at==j->length);
    schema(j,bag);free(j);free(bytes);
}
int main(void) {
    static const char source_bytes[]="\t\xc3\xa9\r\n\xf0\x9f\x99\x82\n";
    static const char special[]={'a',0,'"','\\','\n','\t',0x7f,(char)0xc3,(char)0xa9};
    TfSource a,b,raw;TfDiagnosticBag *bag;uint32_t random=17;
    CHECK(tf_source_init(&a,"one.tfd",source_bytes,sizeof(source_bytes)-1,NULL)==TF_OK);
    CHECK(tf_source_init(&b,"other.tfd","",0,NULL)==TF_OK);
    CHECK(tf_source_init(&raw,"invalid.tfd","\xff\x80\0",3,NULL)==TF_SOURCE_ERROR);
    for(size_t trial=0;trial<128;++trial) {
        char message[80];size_t n=0;
        TfDiagnosticText view,notes[2],help[1];TfDiagnosticLabel labels[3];TfDiagnosticEdit edits[2];TfDiagnosticRecord r={0};
        memcpy(message,special,sizeof(special));n=sizeof(special);
        for(size_t i=0;i<trial%48;++i) {random=random*UINT32_C(1664525)+UINT32_C(1013904223);message[n++]=(char)(random%128);}
        view=(TfDiagnosticText){message,n};notes[0]=view;notes[1]=(TfDiagnosticText){NULL,0};help[0]=view;
        labels[0]=(TfDiagnosticLabel){{&a,1,3},true,view};labels[1]=(TfDiagnosticLabel){{&b,0,0},false,view};
        labels[2]=(TfDiagnosticLabel){{&raw,0,3},false,view};
        edits[0]=(TfDiagnosticEdit){{&a,0,sizeof(source_bytes)-1},view};edits[1]=(TfDiagnosticEdit){{&b,0,0},view};
        r.code="E302";r.severity=(TfDiagnosticSeverity)(trial%3);r.message=view;r.labels=labels;r.label_count=3;
        r.notes=notes;r.note_count=2;r.help=help;r.help_count=1;r.edits=edits;r.edit_count=2;
        CHECK(tf_diagnostic_bag_create(&bag,trial%4,NULL)==TF_OK);
        CHECK(tf_diagnostic_bag_add(bag,&r)==TF_OK);CHECK(tf_diagnostic_bag_add(bag,&r)==TF_OK);
        roundtrip(bag);tf_diagnostic_bag_destroy(bag);
    }
    tf_source_destroy(&raw);tf_source_destroy(&b);tf_source_destroy(&a);
    printf("diagnostic JSON decoded schema: %zu checks passed\n",checks);return 0;
}
