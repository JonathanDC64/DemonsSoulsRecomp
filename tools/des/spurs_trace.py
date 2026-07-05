#!/usr/bin/env python3
"""spurs_trace.py -- structured whole-picture analysis of a des_boot log.

Replaces ad-hoc grepping with one pass that reconstructs the SPURS/SPU event
flow, thread liveness, and the queue producer/consumer picture -- the "wide
view" for diagnosing why the SPU->PPU event chain / worker dormancy stalls.

Usage:
  python tools/des/spurs_trace.py <boot_log.txt> [--timeline]

Whitelist: Bash(python tools/des/spurs_trace.py *)

Outputs:
  - THREADS: created vs exited, final wait-state (name, wait obj, waits, lastHLE)
  - SPU: images dispatched / entered / returned
  - EVENT QUEUES: per-queue posts vs receives, selector histogram, first/last line
  - SPURS SUBSTRATE: lv2 substrate creation, real events posted, handler-B entries
  - DORMANCY: threads parked with lastHLE=none (never did work)
  - PANICS: count + top guest frames
  - --timeline: interleaved SPURS-relevant events with line numbers
"""
import sys, re, collections

def main():
    if len(sys.argv) < 2:
        print("usage: spurs_trace.py <boot_log.txt> [--timeline]"); return 1
    path = sys.argv[1]; want_tl = "--timeline" in sys.argv[2:]
    with open(path, "r", errors="replace") as f:
        lines = f.readlines()

    threads_created = {}      # name -> (line, entry, tid)
    threads_exited = set()    # tid
    thr_state = {}            # name -> (wait, obj, waits, lasthle)  (last seen)
    q_recv = collections.Counter()      # queue_id -> recv count
    q_recv_first = {}; q_recv_last = {}
    q_post = collections.Counter()      # queue_id -> post count (real USER events)
    sel_hist = collections.Counter()    # delivered selector histogram
    spu_enter = collections.Counter(); spu_return = collections.Counter()
    substrate = []; handlerB = 0; panics = 0; panic_frames = collections.Counter()
    real_posts = []                     # (line, q, spup, data1)
    timeline = []

    re_thr = re.compile(r'sys_ppu_thread_create name="([^"]+)" entry=0x([0-9A-F]+).*?(?:tid=(\d+))?')
    re_thr2 = re.compile(r'\[SYS\] sys_ppu_thread_create name="([^"]+)" entry=0x([0-9A-F]+)')
    re_exit = re.compile(r'sys_ppu_thread_exit\(tid=(\d+)')
    re_state = re.compile(r'name="([^"]+)" (?:running|WAITING) wait=(\w+) obj=0x([0-9A-F]+) waits=(\d+) lastHLE=([A-Za-z0-9_#()\-]+)')
    re_recv = re.compile(r'event_queue_receive\(q=(\d+)')
    re_evqrecv = re.compile(r'\[evq-recv\] q=(\d+) .*\(sel=(\d+)\)')
    re_post = re.compile(r'POST real USER event q=(\d+) lv2=0x[0-9A-F]+ spup=(\d+) data0=0x[0-9A-F]+ data1=0x([0-9A-F]+)')
    re_spu = re.compile(r'async image=(\d+) (ENTER run|RETURNED)')
    re_frame = re.compile(r'#\d+ 0x([0-9A-F]{8})')

    for i, ln in enumerate(lines, 1):
        m = re_thr2.search(ln)
        if m:
            threads_created[m.group(1)] = (i, m.group(2));
            if want_tl: timeline.append((i, f"THREAD create {m.group(1)} entry=0x{m.group(2)}"))
        m = re_exit.search(ln)
        if m: threads_exited.add(m.group(1))
        m = re_state.search(ln)
        if m: thr_state[m.group(1)] = (m.group(2), m.group(3), int(m.group(4)), m.group(5))
        m = re_recv.search(ln)
        if m:
            q = int(m.group(1)); q_recv[q]+=1
            q_recv_first.setdefault(q,i); q_recv_last[q]=i
        m = re_evqrecv.search(ln)
        if m:
            sel_hist[int(m.group(2))]+=1
            if want_tl: timeline.append((i, f"  RECV q={m.group(1)} sel={m.group(2)}"))
        m = re_post.search(ln)
        if m:
            q=int(m.group(1)); q_post[q]+=1; real_posts.append((i,q,int(m.group(2)),m.group(3)))
            if want_tl: timeline.append((i, f"  POST q={m.group(1)} spup={m.group(2)} data1=0x{m.group(3)}"))
        m = re_spu.search(ln)
        if m:
            if m.group(2).startswith("ENTER"): spu_enter[int(m.group(1))]+=1
            else: spu_return[int(m.group(1))]+=1
        if "P2: lv2 substrate" in ln or "display/vblank event port" in ln:
            substrate.append(ln.strip())
        if "[handlerB]" in ln: handlerB += 1
        if re.search(r'no runnable target|Dantelion2 Panic|Runtime error', ln): panics += 1
        m = re_frame.search(ln)
        if m and panics: pass  # frames counted below via a second pass window

    # panic frames: collect guest frames near backtraces
    for i, ln in enumerate(lines):
        if 'guest backtrace' in ln:
            for j in range(i+1, min(i+11, len(lines))):
                fm = re_frame.search(lines[j])
                if fm: panic_frames[fm.group(1)] += 1

    P = print
    P(f"===== SPURS TRACE: {path} ({len(lines)} lines) =====\n")
    P("--- THREADS (created / exited / final state) ---")
    for name,(cl,entry) in sorted(threads_created.items()):
        st = thr_state.get(name)
        sts = f"wait={st[0]} obj=0x{st[1]} waits={st[2]} lastHLE={st[3]}" if st else "(no state)"
        P(f"  {name:34s} {sts}")
    P(f"\n--- DORMANT (parked, lastHLE=none -- never did work) ---")
    for name,st in sorted(thr_state.items()):
        if st[3] in ("-(#0)","-"): P(f"  {name:34s} wait={st[0]} obj=0x{st[1]} waits={st[2]}")
    P(f"\n--- SPU IMAGES (enter/return) ---")
    for img in sorted(set(spu_enter)|set(spu_return)):
        P(f"  image={img:<3d} enter={spu_enter[img]:<4d} return={spu_return[img]}")
    P(f"\n--- EVENT QUEUES (recv vs real-USER-post) ---")
    for q in sorted(set(q_recv)|set(q_post)):
        P(f"  q={q:<4d} recv={q_recv[q]:<5d} realPost={q_post[q]:<5d} recvLines[{q_recv_first.get(q,'-')}..{q_recv_last.get(q,'-')}]")
    P(f"\n--- DELIVERED SPU-USER-EVENT selectors ---  {dict(sel_hist)}")
    P(f"\n--- SPURS SUBSTRATE ---")
    for s in substrate[:4]: P(f"  {s}")
    P(f"  real USER events posted: {sum(q_post.values())}   handlerB entries: {handlerB}")
    if real_posts:
        f=real_posts[0]; l=real_posts[-1]
        P(f"  first post: line {f[0]} q={f[1]} spup={f[2]} data1=0x{f[3]}   last post: line {l[0]}")
    P(f"\n--- PANICS: {panics}  top guest frames: " + ", ".join(f"0x{a}({c})" for a,c in panic_frames.most_common(6)))
    # KEY DIAGNOSTIC: post-vs-recv timing per posted queue
    P(f"\n--- KEY: does the consumer receive AFTER we post? ---")
    for q in sorted(q_post):
        pf = next((ln for ln,qq,_,_ in real_posts if qq==q), None)
        rl = q_recv_last.get(q,0)
        verdict = "OK (recv after post)" if rl and pf and rl>=pf else "STALE (no recv after post -> events pile up)"
        P(f"  q={q}: firstPost=line{pf} lastRecv=line{rl}  -> {verdict}")
    if want_tl:
        P("\n--- TIMELINE (SPURS-relevant) ---")
        for ln,txt in timeline: P(f"  {ln:7d} {txt}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
