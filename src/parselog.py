import re, collections

log = r"E:\Games\Steam\steamapps\common\Lies of P\LiesofP\Binaries\Win64\ffx_bridge.log"

call_re  = re.compile(r"^>>> (ffx\w+)\((.*)\)")
ret_re   = re.compile(r"^<<< (ffx\w+) -> (\d+)(.*)")
node_re  = re.compile(r"^\s+#(\d+) type=0x([0-9a-fA-F]+) effect=(\S+)\s+pNext=(\S+)")
hex_re   = re.compile(r"^\s+\+([0-9a-f]+): (.*)")

# counts of (api, top-level-type)
counts = collections.Counter()
# first full block text for each (api, set-of-types-in-chain signature) keyed by (api, type0)
first_block = {}
# ordered setup calls (create/configure/destroy/query) - skip dispatch to keep small
setup = []

cur_api = None
cur_args = None
cur_lines = []
cur_types = []     # list of (idx,type,effect)
in_call = False

def flush():
    global cur_api, cur_lines, cur_types
    if not cur_api: return
    top = cur_types[0][1] if cur_types else None
    key = (cur_api, top)
    counts[key] += 1
    if key not in first_block:
        first_block[key] = "\n".join(cur_lines)
    if cur_api in ("ffxCreateContext","ffxConfigure","ffxDestroyContext","ffxQuery"):
        # record concise setup line: api + chain types
        sig = " | ".join(f"{e}:0x{t}" for _,t,e in cur_types) if cur_types else "(no desc)"
        if len(setup) < 400:
            setup.append(f"{cur_api}: {sig}")
    cur_api=None; cur_lines=[]; cur_types=[]

with open(log, "r", errors="replace") as f:
    for line in f:
        line = line.rstrip("\n")
        m = call_re.match(line)
        if m:
            flush()
            cur_api = m.group(1); cur_args = m.group(2)
            cur_lines = [line]; cur_types = []
            continue
        if cur_api:
            cur_lines.append(line)
            nm = node_re.match(line)
            if nm:
                cur_types.append((int(nm.group(1)), nm.group(2).lower(), nm.group(3)))
            if ret_re.match(line):
                # keep return line then flush
                flush()
flush()

print("================ CALL COUNTS by (api, top type) ================")
for (api,t),c in sorted(counts.items(), key=lambda kv:(-kv[1])):
    print(f"  {c:>8}  {api:18} type=0x{t}")

print("\n================ SETUP SEQUENCE (create/configure/query/destroy, first 120) ================")
for s in setup[:120]:
    print("  ", s)

print("\n================ FIRST FULL BLOCK per (api, top type) ================")
# Print create/configure/query blocks fully; for dispatch print too (one sample each)
order = sorted(first_block.keys(), key=lambda k:(k[0], k[1] or ""))
for key in order:
    api,t = key
    print(f"\n----- {api}  type=0x{t}  (count={counts[key]}) -----")
    txt = first_block[key]
    # cap block length
    lines = txt.split("\n")
    for ln in lines[:60]:
        print(ln)
    if len(lines) > 60:
        print(f"   ... (+{len(lines)-60} more lines)")
