/* Bounded JSON for the example manifest and compiler diagnostic observations.
 * No sentinel reads: decoded strings retain their explicit byte lengths. */
#ifndef FIXTURE_JSON_H
#define FIXTURE_JSON_H
#include <stdint.h>

enum { JSON_TOKENS = 32768, JSON_DEPTH = 32, JSON_BYTES = 1024 * 1024 };
typedef struct { size_t start, end, next; char type; } JToken;
typedef struct {
    const char *s;
    size_t length, at, count;
    JToken *t;
} Json;

static int jpeek(const Json *j) {
    return j->at < j->length ? (unsigned char)j->s[j->at] : -1;
}
static void jspace(Json *j) {
    while (jpeek(j)==' ' || jpeek(j)=='\n' || jpeek(j)=='\r' || jpeek(j)=='\t') ++j->at;
}
static int jhex(int c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static bool junit(Json *j, uint32_t *u) {
    *u=0;
    for (unsigned i=0;i<4;++i) {
        const int h=jhex(jpeek(j));
        if (h<0) return false;
        *u=*u*16u+(uint32_t)h; ++j->at;
    }
    return true;
}
/* Called after an opening quote, also used when validating string tokens.
 * The output is never longer than the encoded input. */
static bool jstring(Json *j, char *out_bytes, size_t *length) {
    size_t n=0;
    while (jpeek(j)!='"') {
        int c=jpeek(j);
        if (c<32) return false;
        ++j->at;
        uint32_t u=(uint32_t)c;
        if (c=='\\') {
            c=jpeek(j);
            if (c<0) return false;
            ++j->at;
            switch (c) {
                case '"': case '\\': case '/': u=(uint32_t)c; break;
                case 'b': u=8; break;
                case 'f': u=12; break;
                case 'n': u=10; break;
                case 'r': u=13; break;
                case 't': u=9; break;
                case 'u': {
                    if (!junit(j,&u)) return false;
                    if (u>=0xd800u && u<=0xdbffu) {
                        if (jpeek(j)!='\\') return false;
                        ++j->at;
                        if (jpeek(j)!='u') return false;
                        ++j->at;
                        uint32_t low;
                        if (!junit(j,&low) || low<0xdc00u || low>0xdfffu) return false;
                        u=0x10000u+(u-0xd800u)*1024u+low-0xdc00u;
                    } else if (u>=0xdc00u && u<=0xdfffu) return false;
                    break;
                }
                default: return false;
            }
        } else if (c>=128) {
            const unsigned count=c>=0xc2 && c<=0xdf ? 1u :
                c>=0xe0 && c<=0xef ? 2u : c>=0xf0 && c<=0xf4 ? 3u : 0u;
            if (!count) return false;
            u=(uint32_t)c & (count==1 ? 31u : count==2 ? 15u : 7u);
            for (unsigned k=0;k<count;++k) {
                c=jpeek(j);
                if (c<128 || c>191) return false;
                ++j->at; u=u*64u+((uint32_t)c & 63u);
            }
            if (u<(count==1 ? 0x80u : count==2 ? 0x800u : 0x10000u) ||
                u>0x10ffffu || (u>=0xd800u && u<=0xdfffu)) return false;
        }
        unsigned char bytes[4]; size_t used;
        if (u<128u) { bytes[0]=(unsigned char)u; used=1; }
        else if (u<2048u) {
            bytes[0]=(unsigned char)(0xc0u | (u>>6));
            bytes[1]=(unsigned char)(0x80u | (u & 63u)); used=2;
        } else if (u<65536u) {
            bytes[0]=(unsigned char)(0xe0u | (u>>12));
            bytes[1]=(unsigned char)(0x80u | ((u>>6) & 63u));
            bytes[2]=(unsigned char)(0x80u | (u & 63u)); used=3;
        } else {
            bytes[0]=(unsigned char)(0xf0u | (u>>18));
            bytes[1]=(unsigned char)(0x80u | ((u>>12) & 63u));
            bytes[2]=(unsigned char)(0x80u | ((u>>6) & 63u));
            bytes[3]=(unsigned char)(0x80u | (u & 63u)); used=4;
        }
        if (out_bytes) memcpy(out_bytes+n,bytes,used);
        n+=used;
    }
    ++j->at; *length=n; return true;
}
static bool jdigit(int c) { return c>='0' && c<='9'; }
static bool jvalue(Json *j, unsigned depth) {
    if (depth>JSON_DEPTH || j->count==JSON_TOKENS) return false;
    jspace(j);
    const int c=jpeek(j);
    const size_t id=j->count++;
    j->t[id].start=j->at; j->t[id].type=(char)c;
    if (c=='"') {
        size_t ignored; ++j->at;
        if (!jstring(j,NULL,&ignored)) return false;
    } else if (c=='[' || c=='{') {
        const int close=c=='[' ? ']' : '}'; ++j->at; jspace(j);
        if (jpeek(j)!=close) for (;;) {
            if (c=='{') {
                jspace(j);
                if (jpeek(j)!='"' || !jvalue(j,depth+1)) return false;
                jspace(j); if (jpeek(j)!=':') return false;
                ++j->at;
            }
            if (!jvalue(j,depth+1)) return false;
            jspace(j);
            if (jpeek(j)==close) break;
            if (jpeek(j)!=',') return false;
            ++j->at;
        }
        ++j->at;
    } else if (c=='t' || c=='f' || c=='n') {
        const char *word=c=='t' ? "true" : c=='f' ? "false" : "null";
        const size_t n=strlen(word);
        if (n>j->length-j->at || memcmp(j->s+j->at,word,n)) return false;
        j->at+=n;
    } else {
        if (c=='-') ++j->at;
        if (jpeek(j)=='0') ++j->at;
        else {
            if (jpeek(j)<'1' || jpeek(j)>'9') return false;
            while (jdigit(jpeek(j))) ++j->at;
        }
        if (jpeek(j)=='.') {
            ++j->at; if (!jdigit(jpeek(j))) return false;
            while (jdigit(jpeek(j))) ++j->at;
        }
        if (jpeek(j)=='e' || jpeek(j)=='E') {
            ++j->at;
            if (jpeek(j)=='+' || jpeek(j)=='-') ++j->at;
            if (!jdigit(jpeek(j))) return false;
            while (jdigit(jpeek(j))) ++j->at;
        }
    }
    j->t[id].end=j->at; j->t[id].next=j->count; return true;
}
static bool jparse(Json *j, const char *s, size_t length) {
    j->s=s; j->length=length; j->at=0; j->count=0;
    if (length>JSON_BYTES || !jvalue(j,0)) return false;
    jspace(j); return j->at==length;
}
static Bytes jbytes(const Json *j, size_t id) {
    CHECK(id<j->count && j->t[id].type=='"');
    Bytes b={malloc(j->t[id].end-j->t[id].start+1),0};
    CHECK(b.data!=NULL);
    Json cursor={0}; cursor.s=j->s; cursor.length=j->length; cursor.at=j->t[id].start+1;
    CHECK(jstring(&cursor,b.data,&b.length)); b.data[b.length]=0; return b;
}
static bool jequal(const Json *j, size_t id, const char *s) {
    if (id>=j->count || j->t[id].type!='"') return false;
    Bytes b=jbytes(j,id);
    const bool same=b.length==strlen(s) && memcmp(b.data,s,b.length)==0;
    free(b.data); return same;
}
static size_t jfield(const Json *j, size_t id, const char *key) {
    CHECK(id<j->count && j->t[id].type=='{');
    size_t found=j->count;
    for (size_t i=id+1;i<j->t[id].next;i=j->t[i+1].next) {
        if (jequal(j,i,key)) { CHECK(found==j->count); found=i+1; }
    }
    return found;
}
static size_t jnumber(const Json *j, size_t id) {
    CHECK(id<j->count);
    size_t n=0;
    for (size_t k=j->t[id].start;k<j->t[id].end;++k) {
        CHECK(jdigit((unsigned char)j->s[k]));
        const size_t digit=(size_t)(j->s[k]-'0');
        CHECK(n<=(SIZE_MAX-digit)/10); n=n*10+digit;
    }
    return n;
}
#endif
