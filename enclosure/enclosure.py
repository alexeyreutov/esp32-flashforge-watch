"""Millimetres. Run with cadquery 2.6.1 and trimesh 4.7.4."""
from pathlib import Path
import cadquery as cq
import trimesh

out = Path(__file__).resolve().parent
W, H = 46, 59
rim, lip, cavity = 2.0, 1.6, 17.0
split = lip + cavity
pcb_mounts = [(x, y) for x in (-14.5, 14.5) for y in (-21.1, 21.1)]
case_mounts = [(x, y) for x in (-20, 20) for y in (-26.5, 26.5)]

def block(w,h,d,z,x=0,y=0,r=0):
    s = cq.Workplane("XY").box(w,h,d, centered=(True,True,False))
    if r:
        s = s.edges("|Z").fillet(r)
    return s.translate((x,y,z))

def cylinder(d,h,z,x,y):
    return cq.Workplane("XY").circle(d/2).extrude(h).translate((x,y,z))

# Front face on the build plate. USB is at the negative Y end.
front = block(W,H,split,0,r=3.8)
front = front.cut(block(W-2*rim,H-2*rim,cavity+1,lip,r=2))
# Active area is offset toward the end opposite USB. Add 0.7 mm each side.
front = front.cut(block(32.0,42.2,lip+2,-1,y=1.6,r=0.6))
# Through-ended opening for plug shell and both edge switches.
front = front.cut(block(29,8,split,lip+2,y=-H/2))
for x,y in case_mounts:
    front = front.union(cylinder(5.7,split,0,x,y))
    front = front.cut(cylinder(1.75,9,split-8,x,y))

# Rear cover, flat exterior down for printing. Four separate screws into the
# board's factory threaded bosses set the module height. No load on the LCD.
lid_t = 2.4
post = cavity - 8.2 - 0.4
lid = block(W,H,lid_t,0,r=3.8)
for x,y in pcb_mounts:
    lid = lid.union(cylinder(4.5,post,lid_t,x,y))
    lid = lid.cut(cylinder(2.3,lid_t+post+2,-1,x,y))
for x,y in case_mounts:
    lid = lid.cut(cylinder(2.4,lid_t+2,-1,x,y))
for y in (-10,-5,0,5,10):
    lid = lid.cut(block(19,1.8,lid_t+2,-1,y=y,r=0.8))

for name, part in [('front',front),('rear',lid)]:
    assert part.val().isValid() and len(part.solids().vals()) == 1
    path=out / (name+'.stl')
    cq.exporters.export(part,str(path),tolerance=0.025,angularTolerance=0.12)
    mesh=trimesh.load_mesh(path)
    assert mesh.is_volume and len(mesh.split())==1
    print(name,mesh.extents,'mm',len(mesh.faces),'triangles')

# In assembly coordinates the outside of the rear cover points upward.
assembled_lid=lid.rotate((0,0,0),(0,1,0),180).translate((0,0,split+lid_t))
assert front.intersect(assembled_lid).val().Volume() < 0.0001
assy=cq.Assembly(name='FlashforgeWatch')
assy.add(front,name='Front',color=cq.Color('darkslategray'))
assy.add(assembled_lid,name='Rear',color=cq.Color('slategray'))
assy.save(str(out/'FlashforgeWatch.step'))
