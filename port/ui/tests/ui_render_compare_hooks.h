/* ui_render_compare's counters (ui_render_compare_hooks.c): the D3D12 command list calls each renderer records. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The calls counted. URC_BARRIERS counts the barriers in them, the rest count calls. */
enum {
    URC_RESET, URC_CLOSE,
    URC_DRAW, URC_DRAW_INDEXED,
    URC_SET_PIPELINE, URC_SET_ROOT_SIGNATURE, URC_SET_HEAPS,
    URC_SET_CBV, URC_SET_TABLE, URC_SET_TOPOLOGY, URC_SET_VB, URC_SET_IB,
    URC_SET_TARGETS, URC_SET_BLEND_FACTOR, URC_SET_STENCIL_REF, URC_SET_VIEWPORTS, URC_SET_SCISSORS,
    URC_BARRIER_CALLS, URC_BARRIERS,
    URC_CLEAR_RTV, URC_CLEAR_DSV,
    URC_COPY_TEXTURE, URC_COPY_RESOURCE, URC_COPY_BUFFER, URC_RESOLVE,
    URC_COUNT
};
#define URC_RENDERERS 4

/* The renderer whose calls count now (an index below URC_RENDERERS), or -1 for none. */
extern int g_urc_who;
extern unsigned long long g_urc_count[URC_RENDERERS][URC_COUNT];
extern const char* const g_urc_name[URC_COUNT];

/* Patches the table of list (an ID3D12GraphicsCommandList*), which every list of its device shares. 0 on success. */
int urc_hooks_install(void* list);
/* Puts the table back as it was. */
void urc_hooks_remove(void);

#ifdef __cplusplus
}
#endif
