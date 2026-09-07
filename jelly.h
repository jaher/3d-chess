#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// CPU XPBD finite elements; GL-independent so exactly the same solver runs
// on native and WASM. A voxel-fitted tetrahedral volume embeds the STL skin.
namespace jelly {
struct V {
    float x=0,y=0,z=0;
    float& operator[](int i) { return i==0?x:i==1?y:z; }
    float operator[](int i) const { return i==0?x:i==1?y:z; }
    V operator+(V b) const { return {x+b.x,y+b.y,z+b.z}; }
    V operator-(V b) const { return {x-b.x,y-b.y,z-b.z}; }
    V operator*(float s) const { return {x*s,y*s,z*s}; }
    V& operator+=(V b) { x+=b.x;y+=b.y;z+=b.z;return *this; }
};
inline float dot(V a,V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline V cross(V a,V b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline float length(V a) { return std::sqrt(dot(a,a)); }
constexpr int NX=6,NY=6,NZ=12,NN=(NX+1)*(NY+1)*(NZ+1),NC=NX*NY*NZ;
inline int node(int x,int y,int z) { return x+(NX+1)*(y+(NY+1)*z); }
inline int cell(int x,int y,int z) { return x+NX*(y+NY*z); }
struct Binding { std::array<int,4> ids{}; std::array<float,4> w{}; };
struct Tet { std::array<int,4> ids; std::array<V,4> grad; float volume; };
struct Cage {
    V lo{-.6f,-.6f,-1},hi{.6f,.6f,1},spacing{};
    std::array<V,NN> rest{};
    std::array<float,NN> mass{};
    std::vector<Tet> tets;
    float volume=0;
    // Freudenthal tetrahedra: coordinates descend along the cube diagonal.
    // This binding and the GLSL skinning use the identical ordering.
    Binding bind(V p) const {
        int c[3]; float f[3]; const int dims[3]={NX,NY,NZ};
        for(int k=0;k<3;++k) {
            float q=std::clamp((p[k]-lo[k])/spacing[k],0.f,float(dims[k]));
            c[k]=std::min(int(q),dims[k]-1); f[k]=q-c[k];
        }
        int a=0,b=1,d=2;
        if(f[a]<f[b]) std::swap(a,b);
        if(f[b]<f[d]) std::swap(b,d);
        if(f[a]<f[b]) std::swap(a,b);
        Binding r; r.ids[0]=node(c[0],c[1],c[2]);
        ++c[a]; r.ids[1]=node(c[0],c[1],c[2]);
        ++c[b]; r.ids[2]=node(c[0],c[1],c[2]);
        ++c[d]; r.ids[3]=node(c[0],c[1],c[2]);
        r.w={1-f[a],f[a]-f[b],f[b]-f[d],f[d]}; return r;
    }
    void build(const std::vector<float>& skin={}) {
        if(!skin.empty()) {
            lo={1e9f,1e9f,1e9f};hi={-1e9f,-1e9f,-1e9f};
            for(size_t i=3;i+2<skin.size();i+=6) for(int k=0;k<3;++k) {
                lo[k]=std::min(lo[k],skin[i+k]);hi[k]=std::max(hi[k],skin[i+k]);
            }
        }
        spacing={(hi.x-lo.x)/NX,(hi.y-lo.y)/NY,(hi.z-lo.z)/NZ};
        std::array<bool,NC> solid{};
        if(skin.empty()) solid.fill(true);
        else {
            // Conservative triangle/cell rasterization, then exterior flood
            // fill. Includes every render vertex and the enclosed interior;
            // no expensive tetrahedralization dependency at runtime.
            for(size_t i=0;i+17<skin.size();i+=18) {
                int lower[3],upper[3];
                for(int k=0;k<3;++k) {
                    float mn=std::min({skin[i+3+k],skin[i+9+k],skin[i+15+k]});
                    float mx=std::max({skin[i+3+k],skin[i+9+k],skin[i+15+k]});
                    int n=k==2?NZ:NX;
                    lower[k]=std::clamp(int((mn-lo[k])/spacing[k]),0,n-1);
                    upper[k]=std::clamp(int((mx-lo[k])/spacing[k]),0,n-1);
                }
                for(int z=lower[2];z<=upper[2];++z) for(int y=lower[1];y<=upper[1];++y)
                    for(int x=lower[0];x<=upper[0];++x) solid[cell(x,y,z)]=true;
            }
            std::array<bool,NC> exterior{}; std::vector<std::array<int,3>> todo;
            auto add=[&](int x,int y,int z) {
                int i=cell(x,y,z); if(!solid[i]&&!exterior[i]) {exterior[i]=true;todo.push_back({x,y,z});}
            };
            for(int z=0;z<NZ;++z) for(int y=0;y<NY;++y) for(int x=0;x<NX;++x)
                if(x==0||x==NX-1||y==0||y==NY-1||z==0||z==NZ-1) add(x,y,z);
            for(size_t i=0;i<todo.size();++i) for(int k=0;k<3;++k) for(int s:{-1,1}) {
                auto c=todo[i];c[k]+=s;
                if(c[0]>=0&&c[0]<NX&&c[1]>=0&&c[1]<NY&&c[2]>=0&&c[2]<NZ) add(c[0],c[1],c[2]);
            }
            for(int i=0;i<NC;++i) solid[i]=!exterior[i];
        }
        for(int z=0;z<=NZ;++z) for(int y=0;y<=NY;++y) for(int x=0;x<=NX;++x)
            rest[node(x,y,z)]={lo.x+x*spacing.x,lo.y+y*spacing.y,lo.z+z*spacing.z};
        mass.fill(0);volume=0;tets.clear();
        const int permutations[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
        for(int z=0;z<NZ;++z) for(int y=0;y<NY;++y) for(int x=0;x<NX;++x) {
            if(!solid[cell(x,y,z)]) continue;
            for(const auto& p:permutations) {
                Tet t;int c[3]={x,y,z};t.ids[0]=node(x,y,z);
                for(int k=0;k<3;++k) {++c[p[k]];t.ids[k+1]=node(c[0],c[1],c[2]);}
                V a=rest[t.ids[1]]-rest[t.ids[0]],b=rest[t.ids[2]]-rest[t.ids[0]],d=rest[t.ids[3]]-rest[t.ids[0]];
                float det=dot(a,cross(b,d));t.volume=std::abs(det)/6;
                t.grad[1]=cross(b,d)*(1/det);t.grad[2]=cross(d,a)*(1/det);t.grad[3]=cross(a,b)*(1/det);
                t.grad[0]=(t.grad[1]+t.grad[2]+t.grad[3])*-1;
                for(int id:t.ids) mass[id]+=t.volume*.25f;
                volume+=t.volume;tets.push_back(t);
            }
        }
    }
};
inline std::array<Cage,6> cages;
inline const Cage& cage(int type) {
    Cage& c=cages[std::clamp(type,0,5)];if(c.tets.empty()) c.build();return c;
}
} // namespace jelly

struct JellyMotion {
    // Offset is a diagnostic of the largest displacement, not a driving DOF.
    float offset[3]{},target[3]{};
    bool held=false,anchored=true,awake=false;
    int type=0;
    float accumulator=0,quiet_time=0;
    std::vector<jelly::V> x,velocity;
    jelly::Binding grab{};
    jelly::V grab_origin{};
    void configure(int t,bool pin_base=true) {
        if(type!=t||anchored!=pin_base) {x.clear();velocity.clear();awake=false;}
        type=t;anchored=pin_base;
    }
    void init() {
        if(!x.empty()) return;
        const auto& c=jelly::cage(type);x.assign(c.rest.begin(),c.rest.end());velocity.resize(jelly::NN);
        grab=c.bind({0,0,c.hi.z});grab_origin={0,0,c.hi.z};
    }
    void begin_grab(float px,float py,float pz) {
        init();const auto& c=jelly::cage(type);
        // Nearest current material node chooses a local region, not always
        // the crown. Barycentric skin picking supplies a rest-space point.
        grab=c.bind({px,py,pz});grab_origin={};
        for(int k=0;k<4;++k) grab_origin+=x[grab.ids[k]]*grab.w[k];
    }
    void release() { held=false;for(float& v:target)v=0; }
    void pull(float px,float py,float pz) {
        init();held=awake=true;quiet_time=0;
        float p[3]={px,py,pz};
        for(int k=0;k<3;++k) target[k]=std::isfinite(p[k])?std::clamp(p[k],-1.25f,1.25f):0;
    }
    void impulse(float px,float py,float pz) {
        init();awake=true;quiet_time=0;const auto& c=jelly::cage(type);
        jelly::V v;float p[3]={px,py,pz};
        for(int k=0;k<3;++k)v[k]=std::isfinite(p[k])?std::clamp(p[k],-5.f,5.f):0;
        for(int i=0;i<jelly::NN;++i) {
            float h=(c.rest[i].z-c.lo.z)/(c.hi.z-c.lo.z);
            velocity[i]+=v*(anchored?h*h:(h-.5f));
        }
    }
    jelly::V position(jelly::V p) const {
        if(x.empty()||!awake)return p;
        const auto& c=jelly::cage(type);auto b=c.bind(p);jelly::V d{};
        for(int k=0;k<4;++k)d+=(x[b.ids[k]]-c.rest[b.ids[k]])*b.w[k];
        return p+d;
    }
    // deformation gradient columns F = sum_i x_i (grad N_i)^T
    void deformation(const jelly::Tet& t,jelly::V f[3]) const {
        f[0]=f[1]=f[2]={};
        for(int i=0;i<4;++i) for(int k=0;k<3;++k)f[k]+=x[t.ids[i]]*t.grad[i][k];
    }
    float minimum_jacobian() const {
        if(x.empty())return 1;
        float m=1e9f;for(const auto& t:jelly::cage(type).tets) {
            jelly::V f[3];deformation(t,f);m=std::min(m,jelly::dot(f[0],jelly::cross(f[1],f[2])));
        }return m;
    }
    float volume_ratio() const {
        if(x.empty())return 1;
        const auto& c=jelly::cage(type);float v=0;
        for(const auto& t:c.tets) {jelly::V f[3];deformation(t,f);v+=t.volume*jelly::dot(f[0],jelly::cross(f[1],f[2]));}
        return v/c.volume;
    }
    bool step(float dt) {
        using namespace jelly;
        if(!awake||!std::isfinite(dt)||dt<=0)return held||awake;
        init();const auto& c=cage(type);
        // Fixed 120 Hz, at most 30 substeps on a stalled frame. Never feed
        // a giant dt into constraints or burn seconds catching up a tab.
        accumulator=std::min(accumulator+dt,.25f);
        constexpr float h=1.f/120,mu=32,bulk=3200;
        std::array<float,NN> w{};
        for(int i=0;i<NN;++i)w[i]=c.mass[i]>0&&(!anchored||c.rest[i].z>c.lo.z+.001f)?1/c.mass[i]:0;
        std::vector<std::array<float,2>> lambda(c.tets.size());
        while(accumulator>=h) {
            accumulator-=h;auto previous=x;
            for(int i=0;i<NN;++i) {
                if(!w[i]) {x[i]=c.rest[i];velocity[i]={};continue;}
                // Weak pose tether prevents rigid-frame drift in a free menu
                // body; local elastic behavior is supplied by volume elements.
                velocity[i]+=(c.rest[i]-x[i])*(h*(anchored?.2f:6.f));
                velocity[i]=velocity[i]*std::exp(-2.2f*h);x[i]+=velocity[i]*h;
            }
            std::fill(lambda.begin(),lambda.end(),std::array<float,2>{});
            for(int iteration=0;iteration<5;++iteration) {
                if(held) {
                    V p{},goal=grab_origin+V{target[0],target[1],target[2]};float den=.000025f/(h*h);
                    for(int k=0;k<4;++k){p+=x[grab.ids[k]]*grab.w[k];den+=w[grab.ids[k]]*grab.w[k]*grab.w[k];}
                    V delta=(goal-p)*(1/den);
                    // Limit each constraint projection to avoid inverting
                    // adjacent small tetrahedra on abrupt pointer jumps.
                    float len=length(delta);if(len>.00012f)delta=delta*(.00012f/len);
                    for(int k=0;k<4;++k)x[grab.ids[k]]+=delta*(w[grab.ids[k]]*grab.w[k]);
                }
                for(size_t ti=0;ti<c.tets.size();++ti) {
                    size_t ei=iteration%2?c.tets.size()-1-ti:ti;
                    const Tet& t=c.tets[ei];V f[3];deformation(t,f);
                    V co[3]={cross(f[1],f[2]),cross(f[2],f[0]),cross(f[0],f[1])};
                    float J=dot(f[0],co[0]),norm=std::sqrt(dot(f[0],f[0])+dot(f[1],f[1])+dot(f[2],f[2]));
                    if(norm<1e-8f)continue;
                    V gd[4],gv[4];float ad=1/(mu*t.volume*h*h),av=1/(bulk*t.volume*h*h),dd=ad,vv=av,dv=0;
                    for(int k=0;k<4;++k) {
                        V g=t.grad[k];gd[k]=(f[0]*g.x+f[1]*g.y+f[2]*g.z)*(1/norm);
                        gv[k]=co[0]*g.x+co[1]*g.y+co[2]*g.z;
                        dd+=w[t.ids[k]]*dot(gd[k],gd[k]);vv+=w[t.ids[k]]*dot(gv[k],gv[k]);dv+=w[t.ids[k]]*dot(gd[k],gv[k]);
                    }
                    // Block XPBD solve of stable neo-Hookean shear + bulk
                    // energy. Their forces cancel at F=I (no resting creep).
                    float rd=-norm-ad*lambda[ei][0],rv=-(J-1-mu/bulk)-av*lambda[ei][1];
                    float det=dd*vv-dv*dv,ld=(rd*vv-rv*dv)/det,lv=(rv*dd-rd*dv)/det;
                    lambda[ei][0]+=ld;lambda[ei][1]+=lv;
                    for(int k=0;k<4;++k)x[t.ids[k]]+=(gd[k]*ld+gv[k]*lv)*w[t.ids[k]];
                }
            }
            // Backtracking on the whole displacement maintains positive
            // element orientation, unlike clamping individual surface verts.
            for(int guard=0;guard<10&&minimum_jacobian()<.18f;++guard)
                for(int i=0;i<NN;++i)x[i]=(x[i]+previous[i])*.5f;
            float maxv=0,maxd=0;offset[0]=offset[1]=offset[2]=0;
            for(int i=0;i<NN;++i) {
                velocity[i]=(x[i]-previous[i])*(1/h);
                V d=x[i]-c.rest[i];maxv=std::max(maxv,length(velocity[i]));maxd=std::max(maxd,length(d));
                for(int k=0;k<3;++k)if(std::abs(d[k])>std::abs(offset[k]))offset[k]=d[k];
            }
            quiet_time=(!held&&maxv<.015f&&maxd<.008f)?quiet_time+h:0;
            if(quiet_time>.25f) {awake=false;x.assign(c.rest.begin(),c.rest.end());std::fill(velocity.begin(),velocity.end(),V{});for(float& v:offset)v=0;accumulator=0;break;}
        }
        return awake;
    }
};

inline std::array<float,3> jelly_position(const JellyMotion& j,float x,float y,float z) {
    auto p=j.position({x,y,z});return {p.x,p.y,p.z};
}
