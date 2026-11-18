#include "../src/core/primitive_rules.h"

#include <stdlib.h>

static size_t checks;
#define CHECK(ok) do { ++checks; if (!(ok)) { \
    fprintf(stderr,"test_core_primitives:%d: %s\n",__LINE__,#ok); exit(1); \
} } while (0)

int main(void) {
    const TfCorePrimitiveClass integer = TF_CORE_PRIMITIVE_INT, real = TF_CORE_PRIMITIVE_FLOAT;
    const TfCorePrimitiveClass boolean = TF_CORE_PRIMITIVE_BOOL, text = TF_CORE_PRIMITIVE_TEXT;
    const TfCorePrimitiveClass list = TF_CORE_PRIMITIVE_LIST, nominal = TF_CORE_PRIMITIVE_OTHER;
    CHECK(tf_core_primitive_unary(TF_CORE_UNARY_NEGATE,integer,integer));
    CHECK(tf_core_primitive_unary(TF_CORE_UNARY_NEGATE,real,real));
    CHECK(tf_core_primitive_unary(TF_CORE_UNARY_NOT,boolean,boolean));
    CHECK(!tf_core_primitive_unary(TF_CORE_UNARY_NEGATE,boolean,boolean));
    CHECK(!tf_core_primitive_unary(TF_CORE_UNARY_NEGATE,integer,real));
    CHECK(!tf_core_primitive_unary(TF_CORE_UNARY_NOT,integer,integer));
    CHECK(!tf_core_primitive_unary((TfCoreUnary)0,integer,integer));
    for (size_t a=0; a<2; ++a) for (size_t b=0; b<2; ++b) {
        const TfCorePrimitiveClass left = a == 0 ? integer : real;
        const TfCorePrimitiveClass right = b == 0 ? integer : real;
        CHECK(tf_core_primitive_binary(TF_CORE_BINARY_REAL_DIVIDE,left,right,real,a==b,a==1,true));
        CHECK(!tf_core_primitive_binary(TF_CORE_BINARY_REAL_DIVIDE,left,right,integer,a==b,a==0,true));
    }
    CHECK(!tf_core_primitive_binary(TF_CORE_BINARY_REAL_DIVIDE,boolean,integer,real,false,false,true));
    CHECK(!tf_core_primitive_binary(TF_CORE_BINARY_ADD,integer,real,real,false,false,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_ADD,real,real,real,true,true,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_SUBTRACT,integer,integer,integer,true,true,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_MULTIPLY,integer,integer,integer,true,true,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_FLOOR_DIVIDE,integer,integer,integer,true,true,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_REMAINDER,integer,integer,integer,true,true,true));
    CHECK(!tf_core_primitive_binary(TF_CORE_BINARY_FLOOR_DIVIDE,real,real,real,true,true,true));
    CHECK(!tf_core_primitive_binary(TF_CORE_BINARY_REMAINDER,integer,real,integer,false,true,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_POWER,integer,integer,integer,true,true,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_POWER,real,integer,real,false,true,true));
    CHECK(!tf_core_primitive_binary(TF_CORE_BINARY_POWER,real,real,real,true,true,true));
    CHECK(!tf_core_primitive_binary(TF_CORE_BINARY_POWER,real,integer,integer,false,false,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_LESS,real,real,boolean,true,false,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_GREATER,integer,integer,boolean,true,false,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_LESS_EQUAL,real,real,boolean,true,false,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_GREATER_EQUAL,integer,integer,boolean,true,false,true));
    CHECK(!tf_core_primitive_binary(TF_CORE_BINARY_GREATER_EQUAL,real,integer,boolean,false,false,true));
    CHECK(!tf_core_primitive_binary(TF_CORE_BINARY_LESS,text,text,boolean,true,false,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_EQUAL,nominal,nominal,boolean,true,false,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_NOT_EQUAL,list,list,boolean,true,false,true));
    CHECK(!tf_core_primitive_binary(TF_CORE_BINARY_EQUAL,nominal,nominal,boolean,false,false,true));
    CHECK(!tf_core_primitive_binary(TF_CORE_BINARY_EQUAL,nominal,nominal,boolean,true,false,false));
    CHECK(!tf_core_primitive_binary(TF_CORE_BINARY_NOT_EQUAL,integer,integer,integer,true,true,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_CONCAT,text,text,text,true,true,true));
    CHECK(tf_core_primitive_binary(TF_CORE_BINARY_CONCAT,list,list,list,true,true,true));
    CHECK(!tf_core_primitive_binary(TF_CORE_BINARY_CONCAT,list,list,list,false,true,true));
    CHECK(!tf_core_primitive_binary(TF_CORE_BINARY_CONCAT,list,list,list,true,false,true));
    CHECK(!tf_core_primitive_binary((TfCoreBinary)0,boolean,boolean,boolean,true,true,true));
    CHECK(!tf_core_primitive_binary((TfCoreBinary)(TF_CORE_BINARY_CONCAT+1),boolean,boolean,boolean,true,true,true));
    CHECK(!tf_core_primitive_binary((TfCoreBinary)-1,integer,integer,integer,true,true,true));
    printf("Core primitive signatures: %zu checks passed\n",checks);
    return 0;
}
