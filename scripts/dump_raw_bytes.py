import struct

with open('../temp/KoreaObj.LOD', 'rb') as f:
    data = f.read()

LOD_OFFSET = 13919840
LOD_SIZE = 11368
buf = data[LOD_OFFSET:LOD_OFFSET+LOD_SIZE]

tag_count = struct.unpack_from('<I', buf, 0)[0]
data_start = 4 + tag_count * 4
print(f"tagListLength = {tag_count}, data_start = {data_start}")

# BRoot at data_start (offset 0 in node data)
base = data_start
subtree_off = struct.unpack_from('<i', buf, base+32)[0]
print(f"\nBRoot: subTree={subtree_off}")

# BSplitterNode at subtree_off
sp_base = data_start + subtree_off
print(f"\nBSplitterNode at node-data offset {subtree_off}:")
for i in range(0, 48, 16):
    hex_bytes = ' '.join(f'{buf[sp_base+i+j]:02x}' for j in range(min(16, 48-i)))
    print(f"  +{i:3d}: {hex_bytes}")

sib2 = struct.unpack_from('<i', buf, sp_base+4)[0]
A = struct.unpack_from('<f', buf, sp_base+8)[0]
B = struct.unpack_from('<f', buf, sp_base+12)[0]
C = struct.unpack_from('<f', buf, sp_base+16)[0]
D = struct.unpack_from('<f', buf, sp_base+20)[0]
front = struct.unpack_from('<i', buf, sp_base+24)[0]
back = struct.unpack_from('<i', buf, sp_base+28)[0]
print(f"  sibling={sib2} plane=({A:.3f},{B:.3f},{C:.3f},{D:.3f}) front={front} back={back}")

# Check what's at the sibling offset
print(f"\n--- Checking sibling at offset {sib2} ---")
sib_base = data_start + sib2
vt = struct.unpack_from('<I', buf, sib_base)[0]
print(f"  vtable=0x{vt:08X}")
for i in range(0, 96, 16):
    hex_bytes = ' '.join(f'{buf[sib_base+i+j]:02x}' for j in range(16))
    print(f"  +{i:3d}: {hex_bytes}")

# The BDofNode should have subTree at offset 32. Let's check:
bdof_subtree = struct.unpack_from('<i', buf, sib_base+32)[0]
bdof_dofnum = struct.unpack_from('<i', buf, sib_base+36)[0]
print(f"  If BDofNode: subTree(off32)={bdof_subtree} dofNumber(off36)={bdof_dofnum}")

# But what if it's NOT a BDofNode? Let's check if it's actually a BSwitchNode:
sw_swnum = struct.unpack_from('<i', buf, sib_base+8)[0]
sw_nchild = struct.unpack_from('<i', buf, sib_base+12)[0]
sw_subtrees = struct.unpack_from('<i', buf, sib_base+16)[0]
print(f"  If BSwitchNode: switchNum(off8)={sw_swnum} numChildren(off12)={sw_nchild} subTrees(off16)={sw_subtrees}")

# Let me also check: maybe the BSplitterNode has a DIFFERENT layout.
# What if front/back are at different offsets?
# Let's try reading front from offset 28 and back from offset 32:
front_alt = struct.unpack_from('<i', buf, sp_base+28)[0]
back_alt = struct.unpack_from('<i', buf, sp_base+32)[0]
print(f"\n  ALT: if front at off28={front_alt}, back at off32={back_alt}")

# And what if there's no plane D (only A,B,C)?
front_nod = struct.unpack_from('<i', buf, sp_base+20)[0]
back_nod = struct.unpack_from('<i', buf, sp_base+24)[0]
print(f"  ALT: if no D, front at off20={front_nod}, back at off24={back_nod}")
