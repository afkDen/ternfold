#include "ternfold/ast_dump.h"
#include "internal.h"
#include <string.h>

static bool put(FILE *f,const char *s){return fputs(s,f)!=EOF;}
static bool quoted(FILE *f,const unsigned char *bytes,size_t length){
    size_t at=0;
    if((bytes==NULL&&length!=0)||fputc('"',f)==EOF)return false;
    while(at<length){uint32_t scalar;size_t end;unsigned char c=bytes[at];
        if(c=='"'||c=='\\'){if(fputc('\\',f)==EOF||fputc(c,f)==EOF)return false;++at;}
        else if(c<32||c==127){if(fprintf(f,"\\u%04x",(unsigned)c)<0)return false;++at;}
        else{if(!tf_decode(bytes,length,at,&scalar,&end)||fwrite(bytes+at,1,end-at,f)!=end-at)return false;at=end;}
    }
    return fputc('"',f)!=EOF;
}
static bool key(FILE *f,const char *name){return fprintf(f,",\"%s\":",name)>=0;}
static bool ref(FILE *f,const TfAst *ast,TfNodeId id){
    if(id.index==0)return put(f,"null");
    return tf_ast_get(ast,id)!=NULL&&fprintf(f,"%zu",id.index)>=0;
}
static bool child(FILE *f,const TfAst *a,const char *name,TfNodeId id){return key(f,name)&&ref(f,a,id);}
static bool list(FILE *f,const TfAst *a,const char *name,TfNodeList items){
    if((items.count!=0&&items.items==NULL)||!key(f,name)||!put(f,"["))return false;
    for(size_t i=0;i<items.count;++i)if((i&&!put(f,","))||!ref(f,a,items.items[i]))return false;
    return put(f,"]");
}
static bool text(FILE *f,const char *name,TfAstText value){
    return key(f,name)&&quoted(f,value.data,value.length);
}
static bool flag(FILE *f,const char *name,bool value){return key(f,name)&&put(f,value?"true":"false");}
static bool token(FILE *f,const char *name,TfTokenKind value){
    const char *s=tf_token_name(value);
    return key(f,name)&&quoted(f,(const unsigned char *)s,strlen(s));
}
static bool span(FILE *f,const char *name,TfSpan s){
    return key(f,name)&&fprintf(f,"{\"start_byte\":%zu,\"end_byte\":%zu}",s.start,s.end)>=0;
}
static bool fields(FILE *f,const TfAst *a,const TfAstNode *n){
    switch(n->kind){
    case TF_AST_ERROR:case TF_AST_UNIT:case TF_AST_PATTERN_WILDCARD:case TF_AST_PATTERN_UNIT:return true;
    case TF_AST_MISSING:return token(f,"expected",n->as.missing.expected);
    case TF_AST_PROGRAM:return list(f,a,"declarations",n->as.program.declarations);
    case TF_AST_IMPORT:return text(f,"path",n->as.import_decl.path)&&text(f,"alias",n->as.import_decl.alias);
    case TF_AST_FUNCTION:return text(f,"name",n->as.function.name)&&flag(f,"public",n->as.function.public_decl)&&
        list(f,a,"parameters",n->as.function.parameters)&&child(f,a,"result_type",n->as.function.result_type)&&child(f,a,"body",n->as.function.body);
    case TF_AST_RECORD:case TF_AST_ENUM:return text(f,"name",n->as.aggregate.name)&&flag(f,"public",n->as.aggregate.public_decl)&&list(f,a,"members",n->as.aggregate.members);
    case TF_AST_MACHINE:return text(f,"name",n->as.machine.name)&&flag(f,"public",n->as.machine.public_decl)&&child(f,a,"parameter",n->as.machine.parameter)&&
        child(f,a,"yields",n->as.machine.yields)&&child(f,a,"rejects",n->as.machine.rejects)&&list(f,a,"sections",n->as.machine.sections);
    case TF_AST_PARAMETER:return text(f,"name",n->as.parameter.name)&&child(f,a,"type",n->as.parameter.type);
    case TF_AST_VARIANT:return text(f,"name",n->as.variant.name)&&list(f,a,"types",n->as.variant.types)&&flag(f,"parentheses",n->as.variant.parentheses);
    case TF_AST_TYPE_PATH:return list(f,a,"segments",n->as.type_path.segments)&&list(f,a,"arguments",n->as.type_path.arguments);
    case TF_AST_TYPE_FUNCTION:return list(f,a,"parameters",n->as.type_function.parameters)&&child(f,a,"result",n->as.type_function.result);
    case TF_AST_PATH:return text(f,"name",n->as.path.name)&&list(f,a,"segments",n->as.path.segments);
    case TF_AST_LITERAL:case TF_AST_PATTERN_LITERAL:return token(f,"token",n->as.literal.token)&&text(f,"value",n->as.literal.value)&&flag(f,"negative",n->as.literal.negative)&&span(f,"minus",n->as.literal.minus);
    case TF_AST_GROUP:return child(f,a,"expression",n->as.group.expression);
    case TF_AST_LIST:return list(f,a,"elements",n->as.list.elements);
    case TF_AST_UNARY:return token(f,"operator",n->as.unary.op)&&span(f,"operator_span",n->as.unary.operator_span)&&child(f,a,"operand",n->as.unary.operand);
    case TF_AST_BINARY:return token(f,"operator",n->as.binary.op)&&span(f,"operator_span",n->as.binary.operator_span)&&child(f,a,"left",n->as.binary.left)&&child(f,a,"right",n->as.binary.right);
    case TF_AST_CALL:return child(f,a,"callee",n->as.call.callee)&&list(f,a,"arguments",n->as.call.arguments);
    case TF_AST_ARGUMENT:return flag(f,"named",n->as.argument.named)&&text(f,"name",n->as.argument.name)&&child(f,a,"value",n->as.argument.value);
    case TF_AST_PROJECT:return child(f,a,"base",n->as.project.base)&&text(f,"name",n->as.project.name);
    case TF_AST_NEXT:return text(f,"name",n->as.next.name);
    case TF_AST_BLOCK:return list(f,a,"statements",n->as.block.statements)&&child(f,a,"final_expression",n->as.block.final_expression);
    case TF_AST_LET:return text(f,"name",n->as.binding.name)&&flag(f,"discard",n->as.binding.discard)&&child(f,a,"type",n->as.binding.type)&&child(f,a,"value",n->as.binding.value);
    case TF_AST_IF:return child(f,a,"condition",n->as.conditional.condition)&&child(f,a,"then",n->as.conditional.then_block)&&child(f,a,"else",n->as.conditional.else_block);
    case TF_AST_MATCH:return child(f,a,"value",n->as.matching.value)&&list(f,a,"arms",n->as.matching.arms);
    case TF_AST_ARM:return child(f,a,"pattern",n->as.arm.pattern)&&child(f,a,"body",n->as.arm.body);
    case TF_AST_CLOSURE:return list(f,a,"parameters",n->as.closure.parameters)&&child(f,a,"result_type",n->as.closure.result_type)&&child(f,a,"body",n->as.closure.body);
    case TF_AST_FOR:return text(f,"name",n->as.loop.name)&&child(f,a,"iterable",n->as.loop.iterable)&&child(f,a,"body",n->as.loop.body);
    case TF_AST_INPUT:return text(f,"name",n->as.input.name)&&child(f,a,"type",n->as.input.type)&&flag(f,"from_stdin",n->as.input.from_stdin);
    case TF_AST_OUTPUT:return child(f,a,"value",n->as.output.value)&&flag(f,"to_stdout",n->as.output.to_stdout);
    case TF_AST_STATE:return text(f,"name",n->as.state.name)&&child(f,a,"type",n->as.state.type)&&child(f,a,"value",n->as.state.value);
    case TF_AST_WRITER:return text(f,"name",n->as.writer.name)&&child(f,a,"value",n->as.writer.value);
    case TF_AST_GUARD:return child(f,a,"condition",n->as.guard.condition)&&child(f,a,"rejection",n->as.guard.rejection);
    case TF_AST_EMIT:return child(f,a,"value",n->as.emission.value);
    case TF_AST_PATTERN_BINDER:return text(f,"name",n->as.binder.name);
    case TF_AST_PATTERN_VARIANT:return child(f,a,"path",n->as.pattern_variant.path)&&list(f,a,"arguments",n->as.pattern_variant.arguments)&&flag(f,"parentheses",n->as.pattern_variant.parentheses);
    }
    return false;
}
TfStatus tf_ast_dump_write(FILE *f,const TfAst *a,bool errors,bool json){
    if(f==NULL||a==NULL||ferror(f))return TF_HOST_ERROR;
    if(json){if(!put(f,"{\"schema_version\":1,\"root\":")||!ref(f,a,tf_ast_root(a))||!flag(f,"has_errors",errors)||!put(f,",\"nodes\":["))return TF_HOST_ERROR;}
    else if(fprintf(f,"AST root=%zu has_errors=%s nodes=%zu\n",tf_ast_root(a).index,errors?"true":"false",tf_ast_count(a))<0)return TF_HOST_ERROR;
    for(size_t i=0;i<tf_ast_count(a);++i){
        const TfAstNode *n=tf_ast_at(a,i);const char *kind;
        if(n==NULL||n->id.owner!=a||n->id.index!=i+1||n->span.source!=tf_ast_source(a)||n->span.start>n->span.end||n->span.end>n->span.source->length)return TF_HOST_ERROR;
        kind=tf_ast_kind_name(n->kind);
        if(json&&i&&!put(f,","))return TF_HOST_ERROR;
        if(fprintf(f,"{\"id\":%zu,\"kind\":\"%s\",\"start_byte\":%zu,\"end_byte\":%zu",n->id.index,kind,n->span.start,n->span.end)<0||!fields(f,a,n)||!put(f,json?"}":"}\n"))return TF_HOST_ERROR;
    }
    if(json&&!put(f,"]}\n"))return TF_HOST_ERROR;
    return ferror(f)?TF_HOST_ERROR:TF_OK;
}
