/* Experimental per-session resident matrix table.  The original dispatcher
 * and ABI stay in v66-q2-rpc.c.  Compile this file INSTEAD of that dispatcher,
 * with the same HVX objects; do not link both dispatchers into one module.
 * The caller must serialize every call through a session's single handle. */
#include "v66-q2-resident.h"
#define n20_projection_skel_handle_invoke n20_projection_single_matrix_invoke
#include "v66-q2-rpc.c"
#undef n20_projection_skel_handle_invoke

struct resident_session {
    struct matrix slots[N20_RESIDENT_SLOTS];
    uint64_t allocated_bytes;
    uint32_t selected;
};

static uint64_t slot_bytes(const struct matrix *m) {
    return m->memory ? (uint64_t)m->size + HVX_BYTES - 1 : 0;
}

static int read_slot(union rpc_arg *args, uint32_t *slot) {
    struct n20_resident_slot request;
    if (args[0].buffer.size != sizeof(request) || !args[0].buffer.data) return -1;
    memcpy(&request, args[0].buffer.data, sizeof(request));
    if (request.slot >= N20_RESIDENT_SLOTS) return -2;
    *slot = request.slot;
    return 0;
}

static int resident_upload(struct resident_session *session, union rpc_arg *args) {
    struct matrix *m = &session->slots[session->selected];
    struct upload request;
    if (args[0].buffer.size != sizeof(request) || !args[0].buffer.data ||
        args[1].buffer.size > MAX_UPLOAD || (args[1].buffer.size && !args[1].buffer.data)) return -1;
    memcpy(&request, args[0].buffer.data, sizeof(request));
    if (!request.rows || request.rows > MAX_ROWS || request.rows % HVX_ROWS ||
        !request.columns || request.columns > MAX_COLS || request.columns % Q2_WIDTH) return -2;
    const uint64_t size = (uint64_t)(request.rows / HVX_ROWS) * (request.columns / Q2_WIDTH) * sizeof(struct weight_tile);
    const uint64_t before = slot_bytes(m);
    if (request.offset == 0 && session->allocated_bytes - before + size + HVX_BYTES - 1 > N20_RESIDENT_LIMIT_BYTES)
        return -5;
    const int result = upload(m, args);
    /* The legacy upload frees the old matrix on offset zero, including when a
     * replacement allocation fails. Count actual ownership, not intent. */
    session->allocated_bytes = session->allocated_bytes - before + slot_bytes(m);
    return result;
}

int n20_projection_skel_handle_invoke(uint64_t handle, uint32_t call, union rpc_arg *args) {
    struct resident_session *session = (struct resident_session *)(uintptr_t)handle;
    if (call == RPC_OPEN) {
        session = qurt_malloc(sizeof(*session));
        if (!session) return -1;
        *session = (struct resident_session){0};
        args[2].handle = (uintptr_t)session;
        return 0;
    }
    if (call == RPC_CLOSE) {
        session = (struct resident_session *)(uintptr_t)args[0].handle;
        if (!session) return -1;
        for (uint32_t i = 0; i < N20_RESIDENT_SLOTS; ++i)
            if (session->slots[i].memory) qurt_free(session->slots[i].memory);
        qurt_free(session);
        return 0;
    }
    if (!session) return -1;
    struct matrix *m = &session->slots[session->selected];
    switch (call) {
    case RPC_UPLOAD: return resident_upload(session, args);
    case RPC_PROJECT: return project(m, args, 0);
    case RPC_TOP1: return project(m, args, 1);
    case RPC_FWHT: return fwht(args, 0);
    case RPC_FWHT_BOUNDED: return fwht(args, 1);
    case RPC_RESIDENT_SELECT: {
        uint32_t slot; const int result = read_slot(args, &slot);
        if (!result) session->selected = slot;
        return result;
    }
    case RPC_RESIDENT_EVICT: {
        uint32_t slot; const int result = read_slot(args, &slot);
        if (result) return result;
        m = &session->slots[slot];
        session->allocated_bytes -= slot_bytes(m);
        if (m->memory) qurt_free(m->memory);
        *m = (struct matrix){0};
        return 0;
    }
    case RPC_RESIDENT_STATS: {
        if (args[0].buffer.size != sizeof(struct n20_resident_stats) || !args[0].buffer.data) return -1;
        struct n20_resident_stats stats = {0};
        stats.allocated_bytes = session->allocated_bytes;
        stats.limit_bytes = N20_RESIDENT_LIMIT_BYTES;
        stats.selected_slot = session->selected;
        for (uint32_t i = 0; i < N20_RESIDENT_SLOTS; ++i) {
            const struct matrix *item = &session->slots[i];
            stats.allocated_slots += item->memory != NULL;
            stats.complete_slots += item->memory && item->uploaded == item->size;
        }
        memcpy(args[0].buffer.data, &stats, sizeof(stats));
        return 0;
    }
    default: return -1;
    }
}
