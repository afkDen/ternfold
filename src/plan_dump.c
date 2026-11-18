#include "ternfold/plan_dump.h"
#include "internal.h"

#include <string.h>

static const char *context_name(TfMachineReadContext context) {
    switch (context) {
    case TF_MACHINE_INITIALIZER: return "initializer";
    case TF_MACHINE_WRITER: return "writer";
    case TF_MACHINE_GUARD: return "guard";
    case TF_MACHINE_REJECTION: return "rejection";
    case TF_MACHINE_EMISSION: return "emission";
    }
    return NULL;
}

static const char *read_name(TfReferenceKind kind) {
    switch (kind) {
    case TF_REFERENCE_OLD: return "old";
    case TF_REFERENCE_INPUT: return "input";
    case TF_REFERENCE_CANDIDATE: return "candidate";
    default: return NULL;
    }
}

static bool text(FILE *f, TfAstText name) {
    return name.length == 0 || (name.data != NULL &&
        fwrite(name.data,1,name.length,f) == name.length);
}

static bool name_json(FILE *f, TfAstText name) {
    /* Machine and cell names are lexer-validated ASCII identifiers. */
    return fputc('"',f) != EOF && text(f,name) && fputc('"',f) != EOF;
}

static bool path_json(FILE *f, const char *path) {
    const unsigned char *bytes = (const unsigned char *)path;
    const size_t length = strlen(path);
    if (fputc('"',f) == EOF) return false;
    for (size_t i=0; i<length;) {
        uint32_t scalar;
        size_t end;
        unsigned char c = bytes[i];
        if (c == '"' || c == '\\') {
            if (fputc('\\',f) == EOF || fputc(c,f) == EOF) return false;
            ++i;
        } else if (c<32 || c==127) {
            if (fprintf(f,"\\u%04x",(unsigned)c)<0) return false;
            ++i;
        } else if (tf_decode(bytes,length,i,&scalar,&end)) {
            if (fwrite(bytes+i,1,end-i,f) != end-i) return false;
            i = end;
        } else {
            if (fprintf(f,"\\u%04x",(unsigned)c)<0) return false;
            ++i;
        }
    }
    return fputc('"',f) != EOF;
}

static bool span_json(FILE *f, TfSpan span) {
    return fprintf(f,"{\"start_byte\":%zu,\"end_byte\":%zu}",span.start,span.end) >= 0;
}

static bool clause_json(FILE *f, TfMachineClause clause) {
    return fprintf(f,"{\"node\":%zu,\"expression\":%zu,\"span\":",
                   clause.node.index,clause.expression.index) >= 0 &&
           span_json(f,clause.span) && fputc('}',f) != EOF;
}

static TfSpan expression_span(TfNodeId expression) {
    const TfAstNode *node = tf_ast_get(expression.owner,expression);
    return node != NULL ? node->span : (TfSpan){0};
}

static bool ids(FILE *f, const size_t *items, size_t count) {
    if (fputc('[',f) == EOF) return false;
    for (size_t i=0; i<count; ++i)
        if ((i != 0 && fputc(',',f) == EOF) || fprintf(f,"%zu",items[i]) < 0) return false;
    return fputc(']',f) != EOF;
}

static bool json_plan(FILE *f, const TfMachinePlan *plan) {
    if (fprintf(f,"{\"module\":%zu,\"declaration\":%zu,\"name\":",
                plan->type.module_index,plan->declaration.index) < 0 ||
        !name_json(f,plan->name) || fputs(",\"span\":",f) == EOF || !span_json(f,plan->span) ||
        fprintf(f,",\"input_symbol\":%zu,\"cells\":[",plan->input.index) < 0) return false;
    for (size_t i=0; i<plan->cell_count; ++i) {
        const TfMachineCell *cell = &plan->cells[i];
        if ((i != 0 && fputc(',',f) == EOF) ||
            fprintf(f,"{\"id\":%zu,\"symbol\":%zu,\"name\":",cell->id,cell->symbol.index) < 0 ||
            !name_json(f,cell->name) || fprintf(f,",\"type\":%zu,\"declaration\":",cell->type.index) < 0 ||
            !clause_json(f,cell->declaration) || fputs(",\"writers\":",f) == EOF ||
            !ids(f,cell->writers,cell->writer_count) || fputc('}',f) == EOF) return false;
    }
    if (fputs("],\"writers\":[",f) == EOF) return false;
    for (size_t i=0; i<plan->writer_count; ++i)
        if ((i != 0 && fputc(',',f) == EOF) ||
            fprintf(f,"{\"cell\":%zu,\"clause\":",plan->writers[i].cell) < 0 ||
            !clause_json(f,plan->writers[i].clause) || fputc('}',f) == EOF) return false;
    if (fputs("],\"schedule\":",f) == EOF || !ids(f,plan->schedule,plan->schedule_count) ||
        fputs(",\"reads\":[",f) == EOF) return false;
    for (size_t i=0; i<plan->read_count; ++i) {
        const TfMachineRead *read = &plan->reads[i];
        const char *kind = read_name(read->kind), *context = context_name(read->context);
        if (kind == NULL || context == NULL || (i != 0 && fputc(',',f) == EOF) ||
            fprintf(f,"{\"kind\":\"%s\",\"context\":\"%s\",\"clause_index\":%zu,"
                      "\"symbol\":%zu,\"node\":%zu,\"cell\":",
                    kind,context,read->clause_index,read->symbol.index,read->node.index) < 0) return false;
        if (read->cell == SIZE_MAX) {
            if (fputs("null",f) == EOF) return false;
        } else if (fprintf(f,"%zu",read->cell) < 0) return false;
        if (fputs(",\"span\":",f) == EOF || !span_json(f,read->span) || fputc('}',f) == EOF) return false;
    }
    if (fputs("],\"dependencies\":[",f) == EOF) return false;
    for (size_t i=0; i<plan->dependency_count; ++i) {
        const TfMachineDependency *edge = &plan->dependencies[i];
        if ((i != 0 && fputc(',',f) == EOF) ||
            fprintf(f,"{\"prerequisite\":%zu,\"dependent\":%zu,\"occurrences\":",
                    edge->prerequisite,edge->dependent) < 0 ||
            !ids(f,edge->occurrences,edge->occurrence_count) || fputc('}',f) == EOF) return false;
    }
    if (fputs("],\"guards\":[",f) == EOF) return false;
    for (size_t i=0; i<plan->guard_count; ++i)
        if ((i != 0 && fputc(',',f) == EOF) || fputs("{\"clause\":",f) == EOF ||
            !clause_json(f,plan->guards[i].clause) ||
            fprintf(f,",\"rejection\":%zu,\"rejection_span\":",plan->guards[i].rejection.index) < 0 ||
            !span_json(f,expression_span(plan->guards[i].rejection)) || fputc('}',f) == EOF) return false;
    if (fputs("],\"emissions\":[",f) == EOF) return false;
    for (size_t i=0; i<plan->emission_count; ++i)
        if ((i != 0 && fputc(',',f) == EOF) || !clause_json(f,plan->emissions[i])) return false;
    return fputs("]}",f) != EOF;
}

static bool text_plan(FILE *f, const TfMachinePlan *plan) {
    if (fprintf(f,"machine %zu:",plan->type.module_index) < 0 || !text(f,plan->name) ||
        fprintf(f," node=%zu span=%zu..%zu\n  schedule:",plan->declaration.index,
                plan->span.start,plan->span.end) < 0) return false;
    for (size_t i=0; i<plan->schedule_count; ++i)
        if (fputc(' ',f) == EOF || !text(f,plan->cells[plan->schedule[i]].name)) return false;
    if (fputc('\n',f) == EOF) return false;
    for (size_t i=0; i<plan->cell_count; ++i) {
        const TfMachineCell *cell = &plan->cells[i];
        if (fprintf(f,"  cell %zu ",cell->id) < 0 || !text(f,cell->name) ||
            fprintf(f," type=%zu initializer=%zu writer=%zu\n",cell->type.index,
                    cell->declaration.expression.index,cell->writers[0]) < 0) return false;
    }
    for (size_t i=0; i<plan->dependency_count; ++i) {
        const TfMachineDependency *edge = &plan->dependencies[i];
        if (fputs("  dependency ",f) == EOF || !text(f,plan->cells[edge->prerequisite].name) ||
            fputs(" -> ",f) == EOF || !text(f,plan->cells[edge->dependent].name) ||
            fputs(" reads=",f) == EOF || !ids(f,edge->occurrences,edge->occurrence_count) ||
            fputc('\n',f) == EOF) return false;
    }
    for (size_t i=0; i<plan->read_count; ++i) {
        const TfMachineRead *read = &plan->reads[i];
        const char *kind = read_name(read->kind), *context = context_name(read->context);
        if (kind == NULL || context == NULL ||
            fprintf(f,"  read %zu %s context=%s:%zu symbol=%zu node=%zu span=%zu..%zu\n",
                    i,kind,context,read->clause_index,read->symbol.index,read->node.index,
                    read->span.start,read->span.end) < 0) return false;
    }
    for (size_t i=0; i<plan->guard_count; ++i)
        if (fprintf(f,"  guard %zu predicate=%zu reject-on-false=%zu\n",i,
                    plan->guards[i].clause.expression.index,plan->guards[i].rejection.index) < 0)
            return false;
    for (size_t i=0; i<plan->emission_count; ++i)
        if (fprintf(f,"  emission %zu expression=%zu\n",i,plan->emissions[i].expression.index) < 0)
            return false;
    return true;
}

TfStatus tf_plan_dump_write(FILE *stream, const TfMachineProgram *program, bool json) {
    const TfResolution *resolution;
    if (stream == NULL || tf_diagnostic_phase_kind(tf_machine_program_phase(program)) != TF_DIAG_PHASE_VALID ||
        tf_diagnostic_phase_payload(tf_machine_program_phase(program)) != program) return TF_HOST_ERROR;
    resolution = tf_typed_program_resolution(tf_machine_program_typed(program));
    if (json) {
        if (fputs("{\"schema_version\":1,\"guards_proven\":false,\"modules\":[",stream) == EOF)
            return TF_HOST_ERROR;
        for (size_t i=0; i<tf_resolution_module_count(resolution); ++i) {
            const TfResolvedModule *module = tf_resolution_module_at(resolution,i);
            if ((i != 0 && fputc(',',stream) == EOF) ||
                fprintf(stream,"{\"id\":%zu,\"path\":",module->id.index) < 0 ||
                !path_json(stream,module->source->canonical_path) || fputc('}',stream) == EOF)
                return TF_HOST_ERROR;
        }
        if (fputs("],\"machines\":[",stream) == EOF) return TF_HOST_ERROR;
    } else if (fputs("Machine plan; guards are runtime conditions, not proven assertions.\n",stream) == EOF)
        return TF_HOST_ERROR;
    for (size_t i=0; i<tf_machine_program_count(program); ++i) {
        const TfMachinePlan *plan = tf_machine_program_at(program,i);
        if (plan == NULL || (json && i != 0 && fputc(',',stream) == EOF) ||
            !(json ? json_plan(stream,plan) : text_plan(stream,plan))) return TF_HOST_ERROR;
    }
    if (json && fputs("]}\n",stream) == EOF) return TF_HOST_ERROR;
    return ferror(stream) ? TF_HOST_ERROR : TF_OK;
}
