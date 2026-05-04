#!/usr/bin/env python3
"""Build the Index-Lock Evaluation deck for the supervisor review.

Output:
    results/deck/index_lock_evaluation.pptx
    results/deck/figures/index_lock/*.png

Plot layout per workload: one image with 2 rows x 2 cols.
    Row 1: throughput vs threads (Graviton on the left, Xeon on the right).
    Row 2: bar chart of M ops per second at the architecture's maximum
           thread point, sorted descending so the lock ranking is visible.

Style: student voice, plain wording, no em dashes anywhere.
"""
from pathlib import Path
import pandas as pd
import matplotlib.pyplot as plt
from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.enum.text import PP_ALIGN
from pptx.dml.color import RGBColor

ROOT = Path(__file__).resolve().parent.parent
DECK_DIR = ROOT / 'results' / 'deck'
FIG_DIR  = DECK_DIR / 'figures' / 'index_lock'
FIG_DIR.mkdir(parents=True, exist_ok=True)

WH_LOCKS  = ['default', 'rw', 'tas', 'ttas', 'cas', 'occ', 'occ-opt']
CDS_LOCKS = ['std', 'tas', 'ttas', 'cas', 'ticket']
AVL_LOCKS = CDS_LOCKS

WH_PALETTE  = {'default':'#888','rw':'#444','tas':'tab:blue','ttas':'tab:green',
               'cas':'tab:orange','occ':'tab:purple','occ-opt':'tab:red'}
CDS_PALETTE = {'std':'#888','tas':'tab:blue','ttas':'tab:green','cas':'tab:orange','ticket':'tab:red'}
AVL_PALETTE = CDS_PALETTE

ARCH_PRETTY = {'graviton': 'AWS Graviton (ARM, 8 cores)',
               'xeon':     'Intel Xeon (x86, 16 cores in scope)'}

WORKLOAD_TAGS = {
    ('uniform', 80, 10): 'uniform 80/10/10',
    ('uniform', 90, 5):  'uniform 90/5/5 read heavy',
    ('zipfian', 80, 10): 'zipfian 80/10/10',
    ('zipfian', 20, 40): 'zipfian 20/40/40 write heavy',
}

def workload_tag(row):
    rd, ins = int(row['read_pct']), int(row['insert_pct'])
    return WORKLOAD_TAGS.get((row['dist'], rd, ins),
                             f"{row['dist']} {rd}/{ins}/{100-rd-ins}")

def load(glob, prefix):
    csvs = sorted(ROOT.glob(glob))
    df = pd.concat([pd.read_csv(c, sep=';', decimal=',') for c in csvs], ignore_index=True)
    df['lk'] = df['lock'].str.replace(prefix, '', regex=False)
    df['workload'] = df.apply(workload_tag, axis=1)
    df = df[df.arch != 'apple_m3']
    df = df[~((df.arch == 'xeon') & (df.threads > 16))]
    return df.reset_index(drop=True)

print('Loading CSVs ...')
wh  = load('results/*/wh_compare/wh.csv',          'wh-')
cds = load('results/*/avl_compare/cds_striped.csv','cds-')
avl = load('results/*/avl_compare/cds_avl.csv',    'avl-')
print(f'  wh: {len(wh)} rows, cds: {len(cds)} rows, avl: {len(avl)} rows')


def plot_lines_and_bars(df, locks, palette, title, workload, fname):
    """2x2: top row is line plots vs threads; bottom row is bar chart at maxT.
    Left column is Graviton, right column is Xeon."""
    fig, axes = plt.subplots(2, 2, figsize=(13, 7.5),
                             gridspec_kw={'height_ratios': [3, 2]})
    for col, arch in enumerate(['graviton', 'xeon']):
        sub = df[(df.arch == arch) & (df.workload == workload)]
        ax = axes[0, col]
        for lk in locks:
            g = sub[sub.lk == lk].sort_values('threads')
            if g.empty: continue
            ax.plot(g.threads, g.ops_s/1e6, marker='o', linewidth=2.0,
                    color=palette[lk], label=lk)
        ax.set_title(ARCH_PRETTY[arch], fontsize=11)
        ax.set_xlabel('Threads (log scale)')
        ax.set_ylabel('M ops/s')
        ax.set_xscale('log', base=2)
        ax.grid(True, alpha=0.3)
        ax.legend(fontsize=8, ncol=2, loc='best')
        ax = axes[1, col]
        if sub.empty:
            ax.set_axis_off(); continue
        max_t = int(sub.threads.max())
        s = sub[sub.threads == max_t].copy()
        s['ops_M'] = s.ops_s / 1e6
        s = s.sort_values('ops_M', ascending=False)
        bars = ax.bar(s.lk, s.ops_M,
                      color=[palette[lk] for lk in s.lk],
                      edgecolor='black', linewidth=0.4)
        for bar, val in zip(bars, s.ops_M):
            ax.text(bar.get_x() + bar.get_width()/2, val,
                    f'{val:.1f}', ha='center', va='bottom', fontsize=9)
        ax.set_title(f'Ranking at {max_t}T (M ops/s)', fontsize=10)
        ax.set_ylim(0, max(s.ops_M) * 1.18)
        ax.grid(True, axis='y', alpha=0.3)
        ax.tick_params(axis='x', labelsize=9)
    fig.suptitle(f'{title}, workload: {workload}', fontsize=13)
    fig.tight_layout()
    out = FIG_DIR / fname
    fig.savefig(out, dpi=140, bbox_inches='tight')
    plt.close(fig)
    return out


print('Generating plots ...')
plots = {
    'wh_balanced':    plot_lines_and_bars(wh, WH_LOCKS, WH_PALETTE, 'Wormhole', 'uniform 80/10/10', 'wh_balanced.png'),
    'wh_readheavy':   plot_lines_and_bars(wh, WH_LOCKS, WH_PALETTE, 'Wormhole', 'uniform 90/5/5 read heavy', 'wh_readheavy.png'),
    'wh_writeheavy':  plot_lines_and_bars(wh, WH_LOCKS, WH_PALETTE, 'Wormhole', 'zipfian 20/40/40 write heavy', 'wh_writeheavy.png'),
    'cds_balanced':   plot_lines_and_bars(cds, CDS_LOCKS, CDS_PALETTE, 'libcds StripedMap', 'uniform 80/10/10', 'cds_balanced.png'),
    'cds_zipfian':    plot_lines_and_bars(cds, CDS_LOCKS, CDS_PALETTE, 'libcds StripedMap', 'zipfian 80/10/10', 'cds_zipfian.png'),
    'avl_balanced':   plot_lines_and_bars(avl, AVL_LOCKS, AVL_PALETTE, 'libcds BronsonAVL', 'uniform 80/10/10', 'avl_balanced.png'),
    'avl_writeheavy': plot_lines_and_bars(avl, AVL_LOCKS, AVL_PALETTE, 'libcds BronsonAVL', 'zipfian 20/40/40 write heavy', 'avl_writeheavy.png'),
}
for k, p in plots.items():
    print(f'  wrote {p.relative_to(ROOT)}')


def best_lock_table(df, locks, name):
    rows = []
    for arch in ['graviton', 'xeon']:
        sub = df[df.arch == arch]
        max_t = int(sub.threads.max())
        for wkl in sorted(sub.workload.unique()):
            wsub = sub[(sub.workload == wkl) & (sub.threads == max_t)]
            if wsub.empty: continue
            best = wsub.loc[wsub.ops_s.idxmax()]
            rows.append((name, arch, max_t, wkl, best.lk, round(best.ops_s/1e6, 1)))
    return rows

best_rows = []
best_rows.extend(best_lock_table(wh, WH_LOCKS, 'Wormhole'))
best_rows.extend(best_lock_table(cds, CDS_LOCKS, 'StripedMap'))
best_rows.extend(best_lock_table(avl, AVL_LOCKS, 'BronsonAVL'))


# ===========================================================================
# Build the deck
# ===========================================================================

print('\nBuilding deck ...')
prs = Presentation()
prs.slide_width  = Inches(13.333)
prs.slide_height = Inches(7.5)

BLANK = prs.slide_layouts[6]

NAVY  = RGBColor(0x1F, 0x3A, 0x5F)
GREY  = RGBColor(0x55, 0x55, 0x55)
BLACK = RGBColor(0x10, 0x10, 0x10)


def add_textbox(slide, text, left, top, width, height, *, size=14, bold=False,
                color=BLACK, align=PP_ALIGN.LEFT):
    box = slide.shapes.add_textbox(left, top, width, height)
    tf = box.text_frame
    tf.word_wrap = True
    lines = text if isinstance(text, list) else [text]
    for i, line in enumerate(lines):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.alignment = align
        run = p.add_run()
        run.text = line
        run.font.size = Pt(size)
        run.font.bold = bold
        run.font.color.rgb = color
    return box


def add_title(slide, title, subtitle=None):
    add_textbox(slide, title, Inches(0.4), Inches(0.25), Inches(12.5), Inches(0.7),
                size=26, bold=True, color=NAVY)
    if subtitle:
        add_textbox(slide, subtitle, Inches(0.4), Inches(0.95), Inches(12.5), Inches(0.4),
                    size=13, color=GREY)


def add_image_slide(title, subtitle, image_path, bullets):
    sl = prs.slides.add_slide(BLANK)
    add_title(sl, title, subtitle)
    sl.shapes.add_picture(str(image_path),
                          Inches(0.6), Inches(1.4),
                          width=Inches(11.0))
    if bullets:
        add_textbox(sl, bullets, Inches(0.6), Inches(6.3), Inches(12.1), Inches(1.1),
                    size=12, color=BLACK)
    return sl


# ----- Slide 1: title -----
sl = prs.slides.add_slide(BLANK)
add_textbox(sl, 'Index lock evaluation',
            Inches(0.5), Inches(2.4), Inches(12.3), Inches(1.0),
            size=40, bold=True, color=NAVY, align=PP_ALIGN.CENTER)
add_textbox(sl, 'How lock primitive choice changes throughput inside three concurrent indexes',
            Inches(0.5), Inches(3.4), Inches(12.3), Inches(0.6),
            size=18, color=GREY, align=PP_ALIGN.CENTER)
add_textbox(sl, 'Cross architecture: AWS Graviton (ARM) and Intel Xeon (x86)',
            Inches(0.5), Inches(4.0), Inches(12.3), Inches(0.5),
            size=15, color=GREY, align=PP_ALIGN.CENTER)
add_textbox(sl, ['Andrei Pana', 'Research project review', 'May 2026'],
            Inches(0.5), Inches(5.5), Inches(12.3), Inches(1.0),
            size=14, color=BLACK, align=PP_ALIGN.CENTER)


# ----- Slide 2: project context -----
sl = prs.slides.add_slide(BLANK)
add_title(sl, 'What this project is about')
body = [
    'The goal is to see how different lock primitives perform inside three concurrent indexes,',
    'and whether the answer changes when you move from x86 (Intel Xeon) to ARM (AWS Graviton).',
    '',
    'The three indexes used:',
    '   1. libcds StripedMap. Hash table with N stripes, one lock per stripe.',
    '   2. libcds BronsonAVL. Lock based AVL tree with one monitor lock per node.',
    '   3. Wormhole (Wu et al., FAST 2019). Hybrid trie plus hash plus linked list.',
    '',
    'Lock primitives compared:',
    '   StripedMap and BronsonAVL: std::mutex, tas, ttas, cas, ticket.',
    '   Wormhole: default (upstream untouched), rw, tas, ttas, cas, occ, occ-opt.',
    '',
    'The headline question: does the same lock win across architectures, or does it shift?',
]
add_textbox(sl, body, Inches(0.6), Inches(1.4), Inches(12.1), Inches(5.5),
            size=15, color=BLACK)


# ----- Slide 3: setup -----
sl = prs.slides.add_slide(BLANK)
add_title(sl, 'Setup')

table_data = [
    ['Machine', 'Architecture', 'Cores in scope', 'Notes'],
    ['AWS Graviton', 'ARM Neoverse N1', '8', 'Hypervisor pinned vCPU clocks'],
    ['Intel Xeon (diascld45)', 'x86_64', '16 (out of 48)', 'Capped at 16T to stay on a single socket'],
]
rows, cols = len(table_data), len(table_data[0])
tbl = sl.shapes.add_table(rows, cols, Inches(0.6), Inches(1.4), Inches(12.1), Inches(1.4)).table
for r, row in enumerate(table_data):
    for c, val in enumerate(row):
        cell = tbl.cell(r, c)
        cell.text = val
        for p in cell.text_frame.paragraphs:
            for run in p.runs:
                run.font.size = Pt(13)
                run.font.bold = (r == 0)

workload_table = [
    ['Workload tag', 'Distribution', 'Reads %', 'Inserts %', 'Deletes %'],
    ['balanced',     'uniform',      '80',       '10',         '10'],
    ['read heavy',   'uniform',      '90',       '5',          '5'],
    ['hot key',      'zipfian',      '80',       '10',         '10'],
    ['write heavy',  'zipfian',      '20',       '40',         '40'],
]
rows, cols = len(workload_table), len(workload_table[0])
tbl = sl.shapes.add_table(rows, cols, Inches(0.6), Inches(3.1), Inches(12.1), Inches(1.9)).table
for r, row in enumerate(workload_table):
    for c, val in enumerate(row):
        cell = tbl.cell(r, c)
        cell.text = val
        for p in cell.text_frame.paragraphs:
            for run in p.runs:
                run.font.size = Pt(13)
                run.font.bold = (r == 0)

add_textbox(sl,
    ['Each run is 3 seconds of measurement after 1 second of warmup.',
     'Thread ladder is power of 2 up to the per machine cap (1, 2, 4, 8 on Graviton; 1, 2, 4, 8, 16 on Xeon).',
     'Every plot in this deck shows Graviton in the left panel and Xeon in the right panel for the same workload.',
     'Below each line plot we also show a sorted bar chart at the maximum thread point so the ranking is visible at a glance.'],
    Inches(0.6), Inches(5.2), Inches(12.1), Inches(2.0), size=13, color=BLACK)


# ----- Slide 4: How we built StripedMap -----
sl = prs.slides.add_slide(BLANK)
add_title(sl, 'How we built StripedMap',
          'Adapter at include/indexes/striped_map_index.hpp')
body = [
    'Wraps cds::container::StripedMap from libcds.',
    '',
    '   Hash table with std::list buckets and a fixed array of N stripe locks.',
    '   The lock primitive is the template parameter to cds::container::striped_set::striping<Lock>,',
    '   so we plug any lockbench primitive (tas_lock, ttas_lock, cas_lock, ticket_lock, std::mutex) at compile time.',
    '   We use 65536 stripes. Each operation hashes the key, picks the stripe, and acquires its lock.',
    '   Resize policy is load_factor_resizing<4>: when item count divided by buckets exceeds 4,',
    '   the map takes ALL stripe locks (scoped_full_lock) and rehashes. We size the initial bucket array',
    '   large enough that we do not hit a resize during a benchmark run.',
    '',
    'Why it matters: StripedMap is the simplest of the three indexes. There is no traversal,',
    'each operation acquires exactly one lock, so it isolates the cost of a single lock acquire',
    'plus the time to walk the per stripe std::list.',
]
add_textbox(sl, body, Inches(0.5), Inches(1.4), Inches(12.3), Inches(5.8),
            size=14, color=BLACK)


# ----- Slide 5: How we built BronsonAVL -----
sl = prs.slides.add_slide(BLANK)
add_title(sl, 'How we built BronsonAVL',
          'Adapter at include/indexes/avl_tree_index.hpp')
body = [
    'Wraps cds::container::BronsonAVLTreeMap from libcds. Bronson et al. published this',
    'lock based AVL tree as a balanced concurrent ordered map with optimistic version validated reads.',
    '',
    '   Tree is balanced. Each node carries a monitor lock that we plug in via cds::sync::injecting_monitor<Lock>.',
    '   Reads use a sequence style version counter on each node. Writers take the lock, mutate, then bump the version.',
    '   Writes do hand over hand traversal down the tree, locking at most a small constant number of nodes at a time.',
    '   Memory reclamation is handled by URCU (cds::urcu::gc<general_buffered>). Worker threads attach to the URCU',
    '   manager on first call via a thread_local guard, then detach at thread exit.',
    '',
    'Why it matters: the contention surface is much smaller than StripedMap. The hot lock is wherever the',
    'tree is being mutated, which depends on the key access pattern. This is what makes the zipfian',
    'workloads interesting: under a skewed distribution, a small set of nodes carries most writes.',
]
add_textbox(sl, body, Inches(0.5), Inches(1.4), Inches(12.3), Inches(5.8),
            size=13, color=BLACK)


# ----- Slide 6: How we built Wormhole, overview -----
sl = prs.slides.add_slide(BLANK)
add_title(sl, 'How we built Wormhole, part 1: vendoring and shim design',
          'Adapter at include/indexes/wormhole_index.hpp; shim at third_party/wormhole/wh_lock_shim.{h,cpp}')
body = [
    'Wormhole (Wu et al., FAST 2019) is a hybrid trie plus hash plus linked list ordered concurrent index.',
    'It hard codes its own rwlock and spinlock as 4 byte opaque structs in lib.h. For our experiment we need',
    'to swap them with any of our primitives at compile time.',
    '',
    'Approach:',
    '   1. Vendor wormhole in tree at third_party/wormhole/. The only edits to upstream sources are short',
    '      ifdef gated blocks. With WH_LOCK_SHIM unset, the build is upstream wormhole untouched.',
    '   2. Add wh_lock_shim.h, which redefines struct rwlock and struct spinlock as 128 byte storage arrays',
    '      with extern C function declarations matching upstream signatures (rwlock_lock_read, rwlock_trylock_write_nr, etc.).',
    '   3. Add wh_lock_shim.cpp, a C++ TU that picks LockT via WH_LOCK_<NAME> macro, asserts size and alignment,',
    '      and provides extern C bodies. Each function placement-news the LockT into the shim storage at init time',
    '      and dispatches via small if constexpr templates so the non matching branches are elided.',
    '',
    'Variants built per CMake target:',
    '   default (upstream rwlock, no shim), rw, tas, ttas, cas, occ, occ-opt.',
    '',
    'occ-opt is special: writer side uses the cas lock, but readers walk the leaf without taking the lock at all.',
    'They snapshot a per leaf seqlock counter, read the entries, then validate. On mismatch they retry.',
]
add_textbox(sl, body, Inches(0.5), Inches(1.4), Inches(12.3), Inches(5.8),
            size=12, color=BLACK)


# ----- Slide 7: Wormhole bug story (alignment) -----
sl = prs.slides.add_slide(BLANK)
add_title(sl, 'How we built Wormhole, part 2: the alignment bug',
          'Every shim variant SIGSEGV on Xeon during the very first prefill insert')
body = [
    'Symptom: wh_test_rw, wh_test_cas, wh_test_tas, wh_test_ttas, wh_test_occ, wh_test_occ-opt all crashed in',
    'wormleaf_insert_hs on Xeon. wh_test_default ran clean. The crash was invisible on Apple Silicon.',
    '',
    'Root cause:',
    '   wormleaf_shift_inc and wormleaf_shift_dec use _mm256_load_si256 on leaf->ss (AVX2 aligned load).',
    '   Aligned loads need the source address to be a multiple of 32 bytes.',
    '   With upstream wormhole, leaflock + sortlock take 8 bytes total, so leaf->ss starts at offset 1088 (aligned).',
    '   With our shim, leaflock + sortlock take 256 bytes, pushing leaf->ss to offset 1336 (1336 mod 16 = 8).',
    '   Aligned SIMD load at a misaligned address gives SIGSEGV on x86. NEON tolerates misalignment, so Mac was fine.',
    '',
    'Fix (one line):',
    '   _Alignas(32) struct entry13 hs[WH_KPN]; in struct wormleaf, gated on WH_LOCK_SHIM.',
    '   This adds 8 bytes of padding before hs[], pulls hs to offset 320 and ss to 1344, both 32 byte aligned.',
    '',
    'Failed first attempt worth recording:',
    '   I tried shrinking the shim alignment from alignas(64) to alignas(8). That left ss[] still misaligned',
    '   and introduced UB whenever LockT had alignas(64) atomics inside (occ_lock, ticket_lock).',
    '   Mac wh-cas and wh-rw started hanging at 99 percent CPU in rwlock_trylock_write, even single threaded.',
    '   Reverted in the same change as the proper _Alignas(32) fix.',
]
add_textbox(sl, body, Inches(0.5), Inches(1.4), Inches(12.3), Inches(5.8),
            size=11, color=BLACK)


# ----- Slide 8: Wormhole bug story (optimistic reader) -----
sl = prs.slides.add_slide(BLANK)
add_title(sl, 'How we built Wormhole, part 3: bugs in the optimistic reader',
          'Five separate things had to be fixed before wh-occ-opt was correct and fair')
body = [
    '1. Uninitialized occ_seq.',
    '   slab_alloc_safe returns memory with stale bits. If a fresh leaf came in with an odd seq counter,',
    '   readers spun forever waiting for a writer that never existed. Fix: initialize occ_seq to 0 in wormleaf_alloc.',
    '',
    '2. Missing seq bumps in writer fast paths.',
    '   wormhole_jump_leaf_write and wormhole_split_insert bypass the regular wormleaf_lock_write / unlock_write.',
    '   Without explicit bumps the seq would go odd and never come back to even, leaving the leaf in an',
    '   apparent "writer in progress" state forever. Fix: bump occ_seq on trylock_write_nr success in those paths.',
    '',
    '3. Torn entry13 reads.',
    '   entry13 is an 8 byte packed struct (e1: u16 key prefix, e3: u48 compressed pointer).',
    '   Stock wormleaf_match_hs reads e1 and e3 as separate field accesses. Without the leaflock, a writer',
    '   could clear hs[i].v64 between the two reads. Reader sees e1 == pkey, then reads e3 == 0, derefs NULL,',
    '   SIGSEGV. The seqlock validate would catch it, but only after the segfault.',
    '   Fix: read entry13 as a single atomic v64 load, unpack e1 and e3 from the same snapshot.',
    '',
    '4. kv use after free under optimistic reads.',
    '   kvmap_mm_dup frees old kvs immediately on update. An optimistic reader could hold a kv pointer that',
    '   gets freed mid-read. Fix: custom kvmap_mm with a no-op free for wh-occ-opt. kvs leak for the run',
    '   (~150 MB for a 3 second benchmark, acceptable for measurement). A production implementation would defer free via QSBR.',
    '',
    '5. Allocator asymmetry inflated the wh-occ-opt win.',
    '   Locked variants paid roughly 10 to 30 ns per op for free(); the no-op free variant did not.',
    '   That artificially doubled occ-opt apparent advantage on write heavy zipfian. Fix: WH_FAIR_MM CMake',
    '   option that uses the same no-op free MM for ALL variants. Required for an apples to apples comparison.',
]
add_textbox(sl, body, Inches(0.5), Inches(1.4), Inches(12.3), Inches(5.8),
            size=10, color=BLACK)


# ----- Throughput slides (line + maxT bar) -----
add_image_slide(
    title='Wormhole, balanced workload (uniform 80 percent reads)',
    subtitle='Top: throughput vs threads. Bottom: ranking at the architecture\'s maximum threads.',
    image_path=plots['wh_balanced'],
    bullets=[
        '   On Graviton at 8T, occ-opt finishes on top (32.3 M ops/s). tas and occ are right behind.',
        '   On Xeon at 16T, ttas wins narrowly (39.0 M) with occ-opt almost tied at 38.7 M.',
        '   wh-default (upstream wormhole rwlock) is in the middle of the pack on both machines.',
    ],
)

add_image_slide(
    title='Wormhole, read heavy workload (uniform 90 percent reads)',
    subtitle='Where an optimistic reader is expected to win',
    image_path=plots['wh_readheavy'],
    bullets=[
        '   On Xeon, occ-opt opens a clear gap (43.9 M, vs 40.6 M for ttas at 16T).',
        '   On Graviton, tas, occ-opt and ttas finish within a couple of percent at 8T.',
        '   The optimistic reader is the design that benefits most from a high read percentage, especially on x86.',
    ],
)

add_image_slide(
    title='Wormhole, write heavy workload (zipfian 20 percent reads, 40 inserts, 40 deletes)',
    subtitle='Hot key contention, where readers cannot escape conflict',
    image_path=plots['wh_writeheavy'],
    bullets=[
        '   On Graviton at 8T, tas, ttas and cas pull ahead (around 30 M).',
        '   On Xeon at 16T, occ-opt is still on top (22.5 M) with rw and tas right next to it.',
        '   Even on the workload least favourable to the optimistic path, it does not collapse.',
    ],
)

add_image_slide(
    title='libcds StripedMap, balanced workload (uniform 80 percent reads)',
    subtitle='Per stripe lock contention, no traversal',
    image_path=plots['cds_balanced'],
    bullets=[
        '   On Graviton at 8T, ttas wins (32.8 M) with cas right behind (32.4 M).',
        '   On Xeon at 16T, tas takes the lead (18.4 M) with ttas second (16.1 M).',
        '   Different best lock per architecture even on the simplest workload.',
    ],
)

add_image_slide(
    title='libcds StripedMap, hot key workload (zipfian 80 percent reads)',
    subtitle='A few stripes carry most of the traffic',
    image_path=plots['cds_zipfian'],
    bullets=[
        '   On Graviton at 8T, tas leads (32.0 M), with std::mutex and cas tied just behind.',
        '   On Xeon at 16T, cas wins (13.6 M) with ticket and tas close together.',
        '   The drop from balanced to hot key on Xeon is large, which matches expectations.',
    ],
)

add_image_slide(
    title='libcds BronsonAVL, balanced workload (uniform 80 percent reads)',
    subtitle='Per node monitor lock, much smaller contention surface than StripedMap',
    image_path=plots['avl_balanced'],
    bullets=[
        '   On Graviton at 8T, cas and ttas finish in a dead heat at 9.8 M.',
        '   On Xeon at 16T, tas leads (13.3 M), with ttas and cas within a few percent.',
        '   BronsonAVL absolute throughput is lower than StripedMap because traversal cost is real.',
    ],
)

add_image_slide(
    title='libcds BronsonAVL, write heavy workload',
    subtitle='Tree mutations create more lock acquisition per operation',
    image_path=plots['avl_writeheavy'],
    bullets=[
        '   On Graviton at 8T, cas takes the lead (8.6 M) with ttas and tas close behind.',
        '   On Xeon at 16T, ticket wins (3.4 M), with std::mutex unusually close.',
        '   Different best lock per architecture is exactly the cross arch story we wanted to surface.',
    ],
)


# ----- Best lock per (arch, workload) table -----
sl = prs.slides.add_slide(BLANK)
add_title(sl, 'Best lock per architecture and workload',
          'Top throughput at each architecture\'s maximum thread count in scope')

table_rows = [['Index', 'Architecture', 'Max T', 'Workload', 'Best lock', 'Throughput (M ops per s)']] + [
    [r[0], r[1], str(r[2]), r[3], r[4], f'{r[5]:.1f}'] for r in best_rows
]
rows, cols = len(table_rows), len(table_rows[0])
tbl = sl.shapes.add_table(rows, cols, Inches(0.5), Inches(1.4), Inches(12.3), Inches(5.4)).table
for r, row in enumerate(table_rows):
    for c, val in enumerate(row):
        cell = tbl.cell(r, c)
        cell.text = val
        for p in cell.text_frame.paragraphs:
            for run in p.runs:
                run.font.size = Pt(11)
                run.font.bold = (r == 0)

add_textbox(sl,
    ['Quick observations from the table:',
     '   Graviton and Xeon do not always agree on the best lock for the same workload.',
     '   For Wormhole, occ-opt is the headline winner on read leaning workloads (and on uniform 80/10/10 on Graviton).',
     '   For StripedMap and BronsonAVL, tas and ttas dominate. cas and ticket appear more on Xeon than on Graviton.'],
    Inches(0.5), Inches(6.85), Inches(12.3), Inches(0.6), size=11, color=BLACK)


# ----- Final slide: next steps -----
sl = prs.slides.add_slide(BLANK)
add_title(sl, 'Next steps')
body = [
    '   Add Apple Silicon back to the cross arch comparison, just for Wormhole.',
    '   The M3 P core and E core split makes scaling numbers harder to interpret,',
    '   so the plan is to limit the comparison to the first 8 P cores only.',
    '',
    '   Get sudo access on the Xeon for one short controlled run.',
    '   With setup_cpu.sh (performance governor and turbo off) we can rerun a small slice and',
    '   confirm whether the Xeon variance we currently see is mostly DVFS noise or something deeper.',
    '',
    '   Add a per arch lock latency micro benchmark.',
    '   The current numbers are throughput inside an index. A pure latency view of acquire',
    '   and release on each architecture would close the loop, by separating raw atomic cost',
    '   from contention behaviour.',
    '',
    '   Write up a short paper section on the cross architecture lock ranking story.',
    '   The fact that ttas wins more often on Graviton while cas and ticket show up more on Xeon',
    '   is a concrete result worth landing in writing.',
]
add_textbox(sl, body, Inches(0.5), Inches(1.4), Inches(12.3), Inches(5.8),
            size=14, color=BLACK)


out_path = DECK_DIR / 'index_lock_evaluation.pptx'
prs.save(str(out_path))
print(f'\nDeck written to {out_path.relative_to(ROOT)}')
print(f'Slide count: {len(prs.slides)}')
