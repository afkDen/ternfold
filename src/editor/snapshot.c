#include "ternfold/editor.h"
#include "ternfold/machine_plan.h"
#include "../repl_resolve.h"
#include "../typed_signatures.h"
#include <stdlib.h>
#include <string.h>

struct TfEditorSnapshot {
    TfAllocator allocator;
    TfSource source;
    int64_t version;
    bool cancelled;
    TfSourceManager *manager;
    TfResolution *resolution;
    TfTypedSignatures *signatures;
    TfTypedProgram *typed;
    TfMachineProgram *machines;
    TfDiagnosticBag *source_errors;
    const TfDiagnosticBag *diagnostics;
};
static void *alloc_default(void *c, size_t n) { (void)c; return malloc(n); }
static void free_default(void *c, void *p) { (void)c; free(p); }

bool tf_editor_position(const TfSource *s, size_t byte, TfEditorPosition *out) {
    TfLocation loc;
    size_t units = 0, start;
    if (out == NULL || !tf_source_location(s, byte, &loc)) return false;
    if (byte != 0 && byte < s->length && s->bytes[byte - 1] == '\r' && s->bytes[byte] == '\n') return false;
    start = s->line_scalars[loc.line - 1];
    for (size_t i = start; i < loc.scalar; ++i) {
        unsigned char c = s->bytes[s->scalar_offsets[i]];
        if (c == '\r' || c == '\n') return false;
        units += c >= 0xf0 ? 2u : 1u;
    }
    *out = (TfEditorPosition){loc.line - 1, units};
    return true;
}
bool tf_editor_byte(const TfSource *s, TfEditorPosition p, size_t *out) {
    size_t units = 0;
    if (s == NULL || !s->valid || out == NULL || p.line >= s->line_count) return false;
    for (size_t i = s->line_scalars[p.line]; i <= s->scalar_count; ++i) {
        size_t byte = s->scalar_offsets[i];
        if (units == p.character) { *out = byte; return true; }
        if (byte == s->length || s->bytes[byte] == '\r' || s->bytes[byte] == '\n') return false;
        units += s->bytes[byte] >= 0xf0 ? 2u : 1u;
        if (units > p.character) return false;
    }
    return false;
}
void tf_editor_snapshot_destroy(TfEditorSnapshot *s) {
    if (s == NULL) return;
    TfAllocator a = s->allocator;
    tf_machine_program_destroy(s->machines);
    tf_typed_program_destroy(s->typed);
    tf_signatures_destroy(s->signatures);
    tf_resolution_destroy(s->resolution);
    tf_source_manager_destroy(s->manager);
    tf_diagnostic_bag_destroy(s->source_errors);
    tf_source_destroy(&s->source);
    a.deallocate(a.context, s);
}
TfStatus tf_editor_snapshot_create(TfEditorSnapshot **out, const char *name,
    const void *bytes, size_t length, int64_t version, const char *root,
    const TfAllocator *allocator) {
    TfAllocator a = {NULL, alloc_default, free_default};
    TfEditorSnapshot *s;
    TfStatus status;
    TfResolveOptions options = {root, 100};
    if (out == NULL) return TF_HOST_ERROR;
    *out = NULL;
    if (allocator != NULL) a = *allocator;
    if (a.allocate == NULL || a.deallocate == NULL || name == NULL ||
        (bytes == NULL && length != 0) || version < 0 || length > 1048576) return TF_HOST_ERROR;
    s = a.allocate(a.context, sizeof *s);
    if (s == NULL) return TF_HOST_ERROR;
    memset(s, 0, sizeof *s);
    s->allocator = a;
    s->version = version;
    status = tf_source_init(&s->source, name, bytes, length, &a);
    if (status == TF_HOST_ERROR) goto fail;
    if (status == TF_SOURCE_ERROR) {
        if (tf_diagnostic_bag_create(&s->source_errors, 100, &a) != TF_OK ||
            tf_diagnostic_bag_source(s->source_errors, &s->source) != TF_OK) goto fail;
        s->diagnostics = s->source_errors;
        *out = s;
        return TF_OK;
    }
    if (tf_source_manager_create(&s->manager, &a) != TF_OK ||
        tf_resolve_virtual(&s->resolution, s->manager, &s->source, NULL, &options, &a) != TF_OK) goto fail;
    s->diagnostics = tf_resolution_diagnostics(s->resolution);
    if (tf_diagnostic_bag_has_errors(s->diagnostics)) goto success;
    if (tf_signatures_build(&s->signatures, s->resolution, 100, &a) != TF_OK) goto fail;
    s->diagnostics = tf_signatures_diagnostics(s->signatures);
    if (tf_diagnostic_bag_has_errors(s->diagnostics)) goto success;
    if (tf_check(&s->typed, s->resolution, s->signatures, 100, &a) != TF_OK) goto fail;
    s->diagnostics = tf_typed_program_diagnostics(s->typed);
    if (tf_diagnostic_bag_has_errors(s->diagnostics)) goto success;
    if (tf_machine_program_build(&s->machines, s->typed, 100, &a) != TF_OK) goto fail;
    s->diagnostics = tf_machine_program_diagnostics(s->machines);
success:
    *out = s;
    return TF_OK;
fail:
    tf_editor_snapshot_destroy(s);
    return TF_HOST_ERROR;
}
const TfSource *tf_editor_source(const TfEditorSnapshot *s) { return s != NULL ? &s->source : NULL; }
const TfDiagnosticBag *tf_editor_diagnostics(const TfEditorSnapshot *s, int64_t version) {
    return s != NULL && !s->cancelled && s->version == version ? s->diagnostics : NULL;
}
const TfTypeStore *tf_editor_types(const TfEditorSnapshot *s) {
    return s != NULL ? tf_typed_program_store(s->typed) : NULL;
}
void tf_editor_cancel(TfEditorSnapshot *s) { if (s != NULL) s->cancelled = true; }
TfConcreteTypeId tf_editor_hover(const TfEditorSnapshot *s, int64_t version,
                                TfEditorPosition position, TfEditorRange *range) {
    size_t byte, best = SIZE_MAX;
    TfConcreteTypeId type = {0};
    TfSpan span = {0};
    if (s == NULL || range == NULL || tf_editor_diagnostics(s, version) == NULL ||
        tf_diagnostic_bag_has_errors(s->diagnostics) || s->typed == NULL ||
        !tf_editor_byte(&s->source, position, &byte)) return type;
    const TfResolvedModule *module = tf_resolution_module(s->resolution, tf_resolution_root(s->resolution));
    if (module == NULL || module->ast == NULL) return type;
    for (size_t i = 0; i < tf_ast_count(module->ast); ++i) {
        const TfAstNode *n = tf_ast_at(module->ast, i);
        const TfTypedNode *t;
        if (n->span.start > byte || n->span.end <= byte || n->span.end - n->span.start >= best) continue;
        t = tf_typed_program_node(s->typed, n->id);
        if (t == NULL || t->type.owner == NULL) continue;
        best = n->span.end - n->span.start;
        type = t->type;
        span = n->span;
    }
    if (type.owner != NULL && (!tf_editor_position(&s->source, span.start, &range->start) ||
                              !tf_editor_position(&s->source, span.end, &range->end))) return (TfConcreteTypeId){0};
    return type;
}
