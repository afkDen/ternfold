#ifndef TERNFOLD_RT_H
#define TERNFOLD_RT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native ABI 1 value and ownership slice. Generated C includes this header
 * independently of the compiler's frontend headers. */
typedef enum { TF_OK = 0, TF_FAULT = 1 } tf_status;
typedef size_t tf_span_id; /* zero means no source origin */
typedef size_t tf_type_id;
typedef size_t tf_function_id;

typedef enum {
    TF_VALUE_UNIT = 0, TF_VALUE_INT, TF_VALUE_FLOAT, TF_VALUE_BOOL,
    TF_VALUE_CHAR, TF_VALUE_BOXED
} tf_value_tag;

typedef struct tf_object tf_object;
typedef struct {
    tf_value_tag tag;
    union {
        int64_t integer;
        uint64_t float_bits;
        uint8_t boolean;
        uint32_t character;
        tf_object *object;
    } as;
} tf_value;

/* Type and function IDs index the generated tables from one. Fields, cells,
 * enum tags and payloads use declaration ordinals from zero. UNUSED reserves
 * holes for non-values such as Core's internal tag type. */
typedef enum {
    TF_RT_TYPE_UNUSED = 0, TF_RT_TYPE_UNIT, TF_RT_TYPE_INT, TF_RT_TYPE_FLOAT,
    TF_RT_TYPE_BOOL, TF_RT_TYPE_CHAR, TF_RT_TYPE_TEXT, TF_RT_TYPE_LIST,
    TF_RT_TYPE_RECORD, TF_RT_TYPE_VARIANT, TF_RT_TYPE_MACHINE, TF_RT_TYPE_FUNCTION
} tf_rt_type_kind;

typedef struct { const tf_type_id *types; size_t count; } tf_rt_shape;
typedef struct {
    tf_rt_type_kind kind;
    union {
        tf_type_id element;
        tf_rt_shape fields; /* record fields or machine cells */
        struct { const tf_rt_shape *tags; size_t count; } variants;
    } as;
} tf_rt_type;

typedef struct { tf_type_id type; tf_rt_shape captures; } tf_rt_function;
typedef struct {
    const tf_rt_type *types;
    size_t type_count;
    const tf_rt_function *functions;
    size_t function_count;
} tf_rt_program;

typedef enum {
    TF_R001 = 1, TF_R002, TF_R003, TF_R004, TF_R005, TF_R006
} tf_fault_code;

typedef struct { tf_function_id function; tf_span_id call_span; } tf_call_frame;
typedef struct {
    tf_fault_code code;
    tf_span_id span;
    const tf_call_frame *frames; /* outermost first, preserved during unwind */
    size_t frame_count;
} tf_fault;

typedef struct {
    /* Both callbacks NULL selects malloc/free. Otherwise both are required.
     * Allocations have malloc alignment; callbacks must not reenter runtime. */
    void *(*allocate)(void *user, size_t bytes);
    void (*deallocate)(void *user, void *ptr);
    void *user;
} tf_rt_allocator;

#define TF_RT_DEFAULT_BYTE_BUDGET UINT64_C(268435456)
#define TF_RT_MAX_CALL_DEPTH 1024u /* also the default */
typedef struct {
    tf_rt_allocator allocator;
    size_t byte_budget; /* live boxed allocation bytes; zero is valid */
    size_t call_limit;  /* 0..TF_RT_MAX_CALL_DEPTH */
} tf_rt_config;

/* Stack-allocatable. All fields are runtime-private. Do not copy or move an
 * initialized context. Release all owned values and leave all active frames
 * before reinitializing or ending its lifetime. Contexts and their values
 * are single-threaded; boxed values cannot be shared between contexts. */
typedef struct {
    tf_rt_program program;
    tf_rt_config config;
    tf_type_id scalar_types[TF_VALUE_BOXED];
    size_t live_bytes;
    size_t depth;
    tf_fault fault;
    tf_call_frame frames[TF_RT_MAX_CALL_DEPTH];
} tf_context;

/* Init borrows generated, immutable table storage (including nested arrays)
 * and allocator user data for the context's lifetime. Table arrays must have
 * the declared extent; descriptor validation at use checks logical shape, not
 * arbitrary C pointers. Null config selects defaults. Init failure is R004. */
tf_status tf_context_init(tf_context *ctx, const tf_rt_program *program,
                          const tf_rt_config *config);
/* NULL until fault; the result and its frame view borrow the context. */
const tf_fault *tf_context_fault(const tf_context *ctx);
size_t tf_context_live_bytes(const tf_context *ctx);
/* First fault wins. No new frame enters once faulted. Each successful enter
 * must be paired with one leave, including on fault. */
tf_status tf_context_fail(tf_context *ctx, tf_fault_code code, tf_span_id span);
tf_status tf_context_enter(tf_context *ctx, tf_function_id function, tf_span_id span);
void tf_context_leave(tf_context *ctx);

/* Borrow all inputs. An out-slot must be writable, own no heap reference, and
 * be disjoint from inputs and other outputs. Every successful value out-slot
 * receives one owner; every failed value out-slot receives Unit. Fallible
 * operations stop at an existing context fault. Only retain/construct/project
 * produce new heap owners. To move a value, transfer the slot and clear the
 * source to Unit. Fabricated or dangling C pointers are outside this contract. */
tf_value tf_unit(void);
tf_value tf_int(int64_t value);
tf_value tf_bool(bool value);
tf_status tf_float(tf_context *ctx, tf_span_id span, uint64_t bits, tf_value *out);
tf_status tf_char(tf_context *ctx, tf_span_id span, uint32_t scalar, tf_value *out);
tf_status tf_retain(tf_context *ctx, tf_span_id span, tf_value value, tf_value *out);
/* Consumes and clears one owner. Iterative and allocation-free, even after
 * fault. Safe on a Unit slot. The owning context must still be alive. */
void tf_release(tf_value *owned);
/* Returns 0 for unavailable or invalid types. Pure, including after fault. */
tf_type_id tf_value_type(const tf_context *ctx, tf_value value);

/* Bytes/children must cover the declared length/count if representable. Type,
 * kind, arity, tag/function identity and child types must match the generated
 * layouts. The type/shape checks report R004 and size/budget checks report
 * R003. No child is retained until shape validation and allocation succeed. */
tf_status tf_text(tf_context *ctx, tf_span_id span, tf_type_id type,
                  const unsigned char *bytes, size_t length, tf_value *out);
tf_status tf_list(tf_context *ctx, tf_span_id span, tf_type_id type,
                  const tf_value *items, size_t count, tf_value *out);
tf_status tf_record(tf_context *ctx, tf_span_id span, tf_type_id type,
                    const tf_value *fields, size_t count, tf_value *out);
tf_status tf_machine(tf_context *ctx, tf_span_id span, tf_type_id type,
                     const tf_value *cells, size_t count, tf_value *out);
tf_status tf_variant(tf_context *ctx, tf_span_id span, tf_type_id type,
                     size_t tag, const tf_value *payloads, size_t count, tf_value *out);
tf_status tf_closure(tf_context *ctx, tf_span_id span, tf_function_id function,
                     const tf_value *captures, size_t count, tf_value *out);

/* Views borrow storage from a live box and clear their outputs on failure.
 * Empty arrays return NULL; Text is counted UTF-8, without a terminator. */
tf_status tf_text_view(tf_context *ctx, tf_span_id span, tf_value text,
                       const unsigned char **bytes, size_t *length);
tf_status tf_list_length(tf_context *ctx, tf_span_id span, tf_value list, size_t *length);
tf_status tf_variant_tag(tf_context *ctx, tf_span_id span, tf_value variant, size_t *tag);
tf_status tf_closure_view(tf_context *ctx, tf_span_id span, tf_value closure,
                          tf_function_id *function, const tf_value **captures, size_t *count);
/* Checked low-level projections. Bounds or tag violations are R004; a
 * language-level at builtin must handle its own Option bounds semantics. */
tf_status tf_list_at(tf_context *ctx, tf_span_id span, tf_value list, size_t index,
                      tf_value *out);
tf_status tf_project(tf_context *ctx, tf_span_id span, tf_value aggregate, size_t index,
                     tf_value *out);
tf_status tf_payload(tf_context *ctx, tf_span_id span, tf_value variant, size_t tag,
                     size_t index, tf_value *out);

#ifdef TF_RT_TESTING
/* Overflow injection only. Restore the real nonzero count before releasing
 * any owner. Returns the previous count, or zero on an invalid request. */
uint64_t tf_test_swap_refcount(tf_context *ctx, tf_value value, uint64_t count);
#endif
#endif
