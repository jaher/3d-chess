import struct, json, sys
import numpy as np

GLB="/home/jaherrero/retro_chess/retropc_chess.glb"
OUT="models/retro"

with open(GLB,"rb") as f:
    f.read(12)
    clen,ct=struct.unpack("<I4s",f.read(8)); js=json.loads(f.read(clen))
    blen,bt=struct.unpack("<I4s",f.read(8)); BIN=f.read(blen)

nodes=js["nodes"]; accs=js["accessors"]; bvs=js["bufferViews"]
name2idx={n.get("name"):i for i,n in enumerate(nodes)}
parent={}
for i,n in enumerate(nodes):
    for c in n.get("children",[]): parent[c]=i

def node_matrix(n):
    nd=nodes[n]
    if "matrix" in nd:
        return np.array(nd["matrix"],dtype=np.float64).reshape(4,4).T  # column-major -> row-major
    M=np.eye(4)
    if "scale" in nd: M=np.diag(list(nd["scale"])+[1.0])@M
    if "rotation" in nd:
        x,y,z,w=nd["rotation"]
        R=np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
                    [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
                    [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]])
        T=np.eye(4); T[:3,:3]=R; M=T@M
    if "translation" in nd:
        T=np.eye(4); T[:3,3]=nd["translation"]; M=T@M
    return M

def world_matrix(n):
    M=np.eye(4); i=n
    chain=[]
    while i is not None:
        chain.append(i); i=parent.get(i)
    for i in reversed(chain):   # root .. leaf
        M=M@node_matrix(i)
    return M

CT={5126:('<f',4),5123:('<H',2),5125:('<I',4),5121:('<B',1)}
NC={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}
def read_acc(ai):
    a=accs[ai]; bv=bvs[a["bufferView"]]
    base=bv.get("byteOffset",0)+a.get("byteOffset",0)
    fmt,size=CT[a["componentType"]]; nc=NC[a["type"]]; cnt=a["count"]
    stride=bv.get("byteStride") or size*nc
    out=np.empty((cnt,nc),dtype=np.float64 if a["componentType"]==5126 else np.int64)
    for e in range(cnt):
        off=base+e*stride
        for c in range(nc):
            out[e,c]=struct.unpack_from(fmt,BIN,off+c*size)[0]
    return out

def extract(mesh_idx, node_name, normalize=True):
    nidx=name2idx[node_name]
    W=world_matrix(nidx); R=W[:3,:3]
    prim=js["meshes"][mesh_idx]["primitives"][0]
    pos=read_acc(prim["attributes"]["POSITION"])
    nrm=read_acc(prim["attributes"]["NORMAL"])
    uv =read_acc(prim["attributes"]["TEXCOORD_0"])
    idx=read_acc(prim["indices"]).reshape(-1).astype(int)
    # world transform
    posw=(W@np.c_[pos,np.ones(len(pos))].T).T[:,:3]
    nrmw=(R@nrm.T).T
    nrmw/=np.linalg.norm(nrmw,axis=1,keepdims=True)+1e-12
    if normalize:
        bmin,bmax=posw.min(0),posw.max(0); ctr=(bmin+bmax)*0.5
        ext=float((bmax-bmin).max()); s=2.0/ext if ext>0 else 1.0
        posw=(posw-ctr)*s
    # de-index to triangle soup
    flat=np.empty((len(idx),8),dtype=np.float32)
    flat[:,0:3]=posw[idx]; flat[:,3:6]=nrmw[idx]; flat[:,6:8]=uv[idx]
    return flat

def write_uvmesh(path,flat):
    with open(path,"wb") as fh:
        fh.write(b"UVME"); fh.write(struct.pack("<I",len(flat))); fh.write(flat.tobytes())

if __name__=="__main__":
    # validate against existing King (mesh 6, node Object_16)
    import os
    k=extract(6,"Object_16")
    pos=k[:,0:3]
    print("VALIDATE King: verts",len(k),"bbox",pos.min(0).round(3),pos.max(0).round(3))
    # compare to existing King.uvmesh
    with open(os.path.join(OUT,"King.uvmesh"),"rb") as fh:
        assert fh.read(4)==b"UVME"; vc=struct.unpack("<I",fh.read(4))[0]
        ex=np.frombuffer(fh.read(vc*32),dtype=np.float32).reshape(vc,8)
    print("EXISTING King: verts",vc,"bbox",ex[:,0:3].min(0).round(3),ex[:,0:3].max(0).round(3))
    print("match verts:",len(k)==vc, " pos max-diff:", float(np.abs(np.sort(k[:,1])-np.sort(ex[:,1])).max()))
