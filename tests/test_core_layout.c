#include "../src/core/lower_types.h"
#include "../src/typed_signatures.h"

#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(ok) do { ++checks; if (!(ok)) { \
    fprintf(stderr,"test_core_layout:%d: %s\n",__LINE__,#ok); exit(1); \
} } while (0)

typedef struct { size_t calls, fail, live; } Count;
static void *allocate(void *context, size_t size) {
    Count *c = context;
    if (++c->calls == c->fail) return NULL;
    void *p = malloc(size);
    if (p != NULL) ++c->live;
    return p;
}
static void deallocate(void *context, void *p) {
    Count *c = context;
    CHECK(p != NULL && c->live != 0);
    --c->live; free(p);
}

static bool spells(TfCoreText name, const char *text) {
    const size_t n = strlen(text);
    return name.length == n && (n == 0 || memcmp(name.bytes,text,n) == 0);
}
static const TfCoreType *entry(const TfCoreProgramData *data, TfCoreTypeId id, TfCoreTypeKind kind) {
    CHECK(id != 0 && id <= data->type_count && data->types[id-1].kind == kind);
    return &data->types[id-1];
}
static TfCoreTypeId find(const TfCoreProgramData *data, TfCoreTypeKind kind, const char *name) {
    TfCoreTypeId id = 0;
    for (size_t i=0; i<data->type_count; ++i) {
        const TfCoreType *type = &data->types[i];
        if (type->kind == kind && (name == NULL || spells(type->name,name))) {
            CHECK(id == 0); id = i+1;
        }
    }
    CHECK(id != 0);
    return id;
}

static TfCoreTypeId find_decision(const TfCoreProgramData *data, TfCoreTypeId machine) {
    TfCoreTypeId id = 0;
    for (size_t i=0; i<data->type_count; ++i)
        if (data->types[i].kind == TF_CORE_TYPE_DECISION &&
            data->types[i].as.decision.machine == machine) {
            CHECK(id == 0); id = i+1;
        }
    return id;
}

static void inspect(TfCoreLowerTypes *types, const TfTypedProgram *program) {
    TfCoreProgramData view = {0};
    TfCoreTypeId tag, list, duplicate;
    TfCoreSpanId span;
    const TfTypeStore *store = tf_typed_program_store(program);
    tf_core_lower_type_view(types,&view);
    const size_t source_count = tf_type_store_count(store);
    CHECK(view.type_count == source_count+1);
    for (size_t i=0; i<source_count; ++i)
        CHECK(tf_core_lower_type_id(types,tf_type_store_at(store,i)->id) == i+1);
    CHECK(tf_core_lower_type_id(types,(TfConcreteTypeId){NULL,1}) == 0);
    const TfCoreTypeId char_type = find(&view,TF_CORE_TYPE_CHAR,NULL);
    const TfCoreTypeId int_type = find(&view,TF_CORE_TYPE_INT,NULL);
    const TfCoreTypeId text_type = find(&view,TF_CORE_TYPE_TEXT,NULL);
    const TfCoreTypeId packet_type = find(&view,TF_CORE_TYPE_ENUM,"Packet");
    const TfCoreTypeId box_type = find(&view,TF_CORE_TYPE_RECORD,"PacketBox");
    const TfCoreTypeId feed_type = find(&view,TF_CORE_TYPE_MACHINE,"Feed");
    const TfCoreTypeId decision_type = find_decision(&view,feed_type);
    const TfCoreTypeId existing = find(&view,TF_CORE_TYPE_MACHINE,"Existing");
    const TfCoreType *existing_decision = entry(&view,find_decision(&view,existing),TF_CORE_TYPE_DECISION);
    const TfCoreType *packet = entry(&view,packet_type,TF_CORE_TYPE_ENUM);
    const TfCoreType *empty = entry(&view,find(&view,TF_CORE_TYPE_RECORD,"Empty"),TF_CORE_TYPE_RECORD);
    const TfCoreType *box = entry(&view,box_type,TF_CORE_TYPE_RECORD);
    const TfCoreType *feed = entry(&view,feed_type,TF_CORE_TYPE_MACHINE);
    const TfCoreType *decision = entry(&view,decision_type,TF_CORE_TYPE_DECISION);
    CHECK(empty->as.record.field_count == 0 && empty->as.record.fields == NULL);
    CHECK(packet->as.enumeration.variant_count == 2);
    CHECK(spells(packet->as.enumeration.variants[0].name,"Idle") &&
          packet->as.enumeration.variants[0].payloads.count == 0);
    const TfCoreVariant *payload = &packet->as.enumeration.variants[1];
    CHECK(spells(payload->name,"Data") && payload->payloads.count == 2);
    CHECK(payload->payloads.items[0] == int_type && payload->payloads.items[1] == text_type);
    CHECK(box->as.record.field_count == 2 && box->as.record.fields[0].type == packet_type &&
          box->as.record.fields[1].type == char_type);
    CHECK(feed->as.machine.cell_count == 1 && feed->as.machine.cells[0].type == box_type &&
          spells(feed->as.machine.cells[0].name,"snapshot"));
    CHECK(feed->as.machine.input == int_type && feed->as.machine.output == char_type &&
          feed->as.machine.rejection == text_type);
    CHECK(decision->as.decision.machine == feed_type && decision->as.decision.output == char_type &&
          decision->as.decision.rejection == text_type);
    const TfCoreTypeId expected_list = decision->as.decision.emissions;
    CHECK(expected_list == source_count+1 && entry(&view,expected_list,TF_CORE_TYPE_LIST)->as.element == char_type);
    CHECK(existing_decision->as.decision.emissions <= source_count);
    CHECK(existing_decision->as.decision.emissions ==
          entry(&view,existing,TF_CORE_TYPE_MACHINE)->as.machine.cells[0].type);
    CHECK(feed->span != 0 && feed->span <= view.span_count);
    const TfSpan origin = view.spans[feed->span-1];
    CHECK(origin.source != NULL && origin.end-origin.start > 12 &&
          memcmp(origin.source->bytes+origin.start,"machine Feed",12) == 0);
    CHECK(spells(feed->module,"core-layout.tfd"));
    CHECK(tf_core_lower_list(types,char_type,&list) == TF_OK && list == expected_list);
    CHECK(tf_core_lower_tag(types,packet_type,&tag) == TF_OK && tag == source_count+2);
    CHECK(tf_core_lower_tag(types,packet_type,&duplicate) == TF_OK && duplicate == tag);
    CHECK(tf_core_lower_tag(types,int_type,&duplicate) == TF_HOST_ERROR && duplicate == 0);
    CHECK(tf_core_lower_list(types,tag,&duplicate) == TF_HOST_ERROR && duplicate == 0);
    CHECK(tf_core_lower_tag(types,decision_type,&duplicate) == TF_OK && duplicate == tag+1);
    CHECK(tf_core_lower_span(types,origin,&span) == TF_OK && span == view.span_count+1);
    tf_core_lower_type_view(types,&view);
    CHECK(entry(&view,tag,TF_CORE_TYPE_TAG)->as.tagged == packet_type);
    CHECK(entry(&view,duplicate,TF_CORE_TYPE_TAG)->as.tagged == decision_type);
    CHECK(view.spans[span-1].source == origin.source && view.spans[span-1].start == origin.start);
}

static void growth_rollback(TfCoreLowerTypes *types, Count *counter) {
    TfCoreProgramData view = {0};
    TfCoreTypeId last;
    tf_core_lower_type_view(types,&view);
    const TfCoreTypeId original_int = find(&view,TF_CORE_TYPE_INT,NULL);
    last = original_int;
    for (size_t i=0; i<256; ++i) {
        TfCoreTypeId next;
        tf_core_lower_type_view(types,&view);
        const size_t previous_count = view.type_count;
        counter->fail = counter->calls+1;
        TfStatus status = tf_core_lower_list(types,last,&next);
        counter->fail = 0;
        if (status == TF_OK) { last = next; continue; }
        CHECK(status == TF_HOST_ERROR && next == 0);
        tf_core_lower_type_view(types,&view);
        CHECK(view.type_count == previous_count && entry(&view,original_int,TF_CORE_TYPE_INT) != NULL);
        CHECK(tf_core_lower_list(types,last,&next) == TF_OK && next == previous_count+1);
        tf_core_lower_type_view(types,&view);
        CHECK(entry(&view,next,TF_CORE_TYPE_LIST)->as.element == last);
        return;
    }
    CHECK(false);
}

static TfCoreTypeId check_decision(TfCoreLowerTypes *types, TfCoreTypeId machine,
                                  TfCoreTypeId result) {
    TfCoreProgramData view = {0};
    tf_core_lower_type_view(types,&view);
    CHECK(find_decision(&view,machine) == result);
    const TfCoreType *layout = entry(&view,machine,TF_CORE_TYPE_MACHINE);
    const TfCoreType *decision = entry(&view,result,TF_CORE_TYPE_DECISION);
    CHECK(decision->as.decision.machine == machine &&
          decision->as.decision.output == layout->as.machine.output &&
          decision->as.decision.rejection == layout->as.machine.rejection);
    const TfCoreTypeId list = decision->as.decision.emissions;
    CHECK(entry(&view,list,TF_CORE_TYPE_LIST)->as.element == layout->as.machine.output);
    return list;
}

static void machine_results(TfCoreLowerTypes *types, const TfMachineProgram *machines, Count *c) {
    TfCoreProgramData view = {0};
    tf_core_lower_type_view(types,&view);
    const size_t before = view.type_count;
    const size_t source_count = tf_type_store_count(tf_typed_program_store(tf_machine_program_typed(machines)));
    const TfCoreTypeId quiet = find(&view,TF_CORE_TYPE_MACHINE,"Quiet");
    const TfCoreTypeId uncalled = find(&view,TF_CORE_TYPE_MACHINE,"Uncalled");
    const TfCoreTypeId existing = find(&view,TF_CORE_TYPE_MACHINE,"Existing");
    const TfCoreTypeId feed = find(&view,TF_CORE_TYPE_MACHINE,"Feed");
    const TfCoreTypeId known = find_decision(&view,existing);
    const TfCoreTypeId explicit_result = find_decision(&view,feed);
    const TfCoreTypeId box = find(&view,TF_CORE_TYPE_RECORD,"PacketBox");
    CHECK(find_decision(&view,quiet) == 0 && find_decision(&view,uncalled) == 0);
    for (size_t i=0; i<view.type_count; ++i)
        CHECK(view.types[i].kind != TF_CORE_TYPE_LIST || view.types[i].as.element != box);

    const size_t calls = c->calls;
    CHECK(tf_core_lower_machine_id(types,NULL) == 0);
    for (size_t i=0; i<tf_machine_program_count(machines); ++i) {
        const TfMachinePlan *plan = tf_machine_program_at(machines,i);
        const TfCoreTypeId id = tf_core_lower_machine_id(types,plan);
        const TfCoreText name = entry(&view,id,TF_CORE_TYPE_MACHINE)->name;
        CHECK(name.length == plan->name.length && memcmp(name.bytes,plan->name.data,name.length) == 0);
        TfMachinePlan invalid = *plan;
        invalid.type.declaration_index = 0;
        CHECK(tf_core_lower_machine_id(types,&invalid) == 0);
    }
    TfCoreTypeId result = SIZE_MAX;
    CHECK(tf_core_lower_decision(types,box,&result) == TF_HOST_ERROR && result == 0);
    CHECK(tf_core_lower_decision(types,0,&result) == TF_HOST_ERROR && result == 0);
    CHECK(tf_core_lower_decision(types,SIZE_MAX,&result) == TF_HOST_ERROR && result == 0);
    CHECK(tf_core_lower_decision(NULL,feed,&result) == TF_HOST_ERROR && result == 0);
    CHECK(tf_core_lower_decision(types,feed,NULL) == TF_HOST_ERROR);
    CHECK(known != 0 && known <= source_count && explicit_result != 0 && explicit_result <= source_count);
    CHECK(tf_core_lower_decision(types,feed,&result) == TF_OK && result == explicit_result);
    CHECK(tf_core_lower_decision(types,existing,&result) == TF_OK && result == known);
    CHECK(c->calls == calls);

    CHECK(tf_core_lower_decision(types,quiet,&result) == TF_OK && result == before+1);
    const TfCoreTypeId shared = check_decision(types,existing,known);
    CHECK(check_decision(types,quiet,result) == shared);
    CHECK(tf_core_lower_decision(types,uncalled,&result) == TF_OK && result == before+3);
    const TfCoreTypeId emissions = check_decision(types,uncalled,result);
    CHECK(emissions == before+2);
    const size_t after = c->calls;
    TfCoreTypeId duplicate = 0;
    CHECK(tf_core_lower_decision(types,quiet,&duplicate) == TF_OK && duplicate == before+1);
    CHECK(tf_core_lower_decision(types,uncalled,&duplicate) == TF_OK && duplicate == result);
    CHECK(tf_core_lower_list(types,box,&duplicate) == TF_OK && duplicate == emissions);
    CHECK(c->calls == after);
    tf_core_lower_type_view(types,&view);
    CHECK(view.type_count == before+3);
}

static void decision_growth_rollback(const TfMachineProgram *machines, const char *name,
                                     bool needs_list) {
    /* Fill one row at a time on fresh owners to find a growth boundary,
     * without depending on the vector's initial size or growth factor. */
    for (size_t fill=0; fill<256; ++fill) {
        Count c = {0};
        const TfAllocator a = {&c,allocate,deallocate};
        TfCoreLowerTypes *types = NULL;
        TfCoreProgramData view = {0};
        CHECK(tf_core_lower_types_create(&types,machines,&a) == TF_OK);
        tf_core_lower_type_view(types,&view);
        const TfCoreTypeId machine = find(&view,TF_CORE_TYPE_MACHINE,name);
        TfCoreTypeId element = find(&view,TF_CORE_TYPE_INT,NULL);
        for (size_t i=0; i<fill; ++i) {
            TfCoreTypeId list;
            CHECK(tf_core_lower_list(types,element,&list) == TF_OK);
            element = list;
        }
        tf_core_lower_type_view(types,&view);
        const size_t before = view.type_count;
        const size_t live = c.live;
        TfCoreTypeId decision = SIZE_MAX;
        c.fail = c.calls+1;
        const TfStatus status = tf_core_lower_decision(types,machine,&decision);
        c.fail = 0;
        if (status == TF_OK) {
            tf_core_lower_types_destroy(types);
            CHECK(c.live == 0);
            continue;
        }
        CHECK(status == TF_HOST_ERROR && decision == 0 && c.live == live);
        tf_core_lower_type_view(types,&view);
        CHECK(view.type_count == before && find_decision(&view,machine) == 0);
        CHECK(tf_core_lower_decision(types,machine,&decision) == TF_OK);
        CHECK(decision == before+(needs_list ? 2u : 1u));
        const TfCoreTypeId list = check_decision(types,machine,decision);
        CHECK(needs_list ? list == before+1 : list <= before);
        tf_core_lower_type_view(types,&view);
        CHECK(view.type_count == decision);
        tf_core_lower_types_destroy(types);
        CHECK(c.live == 0);
        return;
    }
    CHECK(false);
}

int main(void) {
    TfSourceManager *manager = NULL;
    TfResolution *resolution = NULL;
    TfTypedSignatures *signatures = NULL;
    TfTypedProgram *program = NULL;
    TfMachineProgram *machines = NULL;
    TfCoreLowerTypes *types = NULL;
    Count c = {0};
    const TfAllocator a = {&c,allocate,deallocate};
    CHECK(tf_source_manager_create(&manager,NULL) == TF_OK);
    CHECK(tf_resolve(&resolution,manager,"tests/fixtures/types/core-layout.tfd",NULL,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_resolution_phase(resolution)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_signatures_build(&signatures,resolution,100,NULL) == TF_OK);
    CHECK(tf_check(&program,resolution,signatures,100,NULL) == TF_OK);
    CHECK(tf_machine_program_build(&machines,program,100,NULL) == TF_OK);
    CHECK(tf_diagnostic_phase_kind(tf_machine_program_phase(machines)) == TF_DIAG_PHASE_VALID);
    CHECK(tf_core_lower_types_create(&types,machines,&a) == TF_OK);
    const size_t injected = c.calls;
    inspect(types,program);
    machine_results(types,machines,&c);
    growth_rollback(types,&c);
    tf_core_lower_types_destroy(types);
    CHECK(c.live == 0 && injected != 0);
    for (size_t i=1; i<=injected; ++i) {
        c = (Count){.fail=i};
        CHECK(tf_core_lower_types_create(&types,machines,&a) == TF_HOST_ERROR);
        CHECK(types == NULL && c.live == 0);
    }
    decision_growth_rollback(machines,"Uncalled",true);
    decision_growth_rollback(machines,"Quiet",false);
    CHECK(tf_core_lower_types_create(&types,NULL,&a) == TF_HOST_ERROR && types == NULL);
    tf_machine_program_destroy(machines);
    tf_typed_program_destroy(program);
    tf_signatures_destroy(signatures);
    tf_resolution_destroy(resolution);
    tf_source_manager_destroy(manager);
    printf("Core layout translation: %zu checks, %zu create failures, 2 Decision growth retries\n",
           checks,injected);
    return 0;
}
