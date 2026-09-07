// Included only by shader.cpp after the platform GLSL preambles.
// Shared tetrahedron ordering with jelly::Cage::bind, including tie breaks.
#define JELLY_SKIN R"(
uniform vec4 uJelly;
uniform highp sampler2D uJellyNodes;
uniform vec3 uJellyLo;
uniform vec3 uJellyInvSpacing;
vec3 nodeDisplacement(ivec3 c) {
    return texelFetch(uJellyNodes,ivec2(c.x+7*(c.y+7*c.z),0),0).xyz;
}
vec3 jellyDeform(vec3 p, out mat3 J) {
    J=mat3(1.0);
    if(uJelly.w<0.5) return p;
    vec3 q=clamp((p-uJellyLo)*uJellyInvSpacing,vec3(0),vec3(6,6,12));
    ivec3 c=min(ivec3(q),ivec3(5,5,11));vec3 f=q-vec3(c);
    int a=0,b=1,d=2,t;
    if(f[a]<f[b]){t=a;a=b;b=t;}if(f[b]<f[d]){t=b;b=d;d=t;}if(f[a]<f[b]){t=a;a=b;b=t;}
    vec3 d0=nodeDisplacement(c);c[a]+=1;vec3 d1=nodeDisplacement(c);
    c[b]+=1;vec3 d2=nodeDisplacement(c);c[d]+=1;vec3 d3=nodeDisplacement(c);
    J[a]+=(d1-d0)*uJellyInvSpacing[a];J[b]+=(d2-d1)*uJellyInvSpacing[b];J[d]+=(d3-d2)*uJellyInvSpacing[d];
    return p+d0+f[a]*(d1-d0)+f[b]*(d2-d1)+f[d]*(d3-d2);
}
vec3 jellyPosition(vec3 p) {mat3 J;return jellyDeform(p,J);}
)"

// Exit normals plus depth24 avoid float-render-target extensions on WebGL2.
const char* jelly_exit_fs_src = GLSL_VERSION GLSL_FS_PREAMBLE R"(
in vec3 vNormal;
out vec4 FragColor;
void main(){FragColor=vec4(normalize(vNormal)*0.5+0.5,1.0);}
)";
const char* jelly_glass_fs_src = GLSL_VERSION GLSL_FS_PREAMBLE R"(
in vec3 vNormal;
in vec3 vFragPos;
out vec4 FragColor;
uniform mat4 uView,uProjection,uInvProjection;
uniform sampler2D uSceneColor,uSceneDepth,uExitNormal,uExitDepth;
uniform vec2 uGlassSize;
uniform int uGlassWhite;
uniform float uGlassIOR;
vec2 projectUV(vec3 p){vec4 q=uProjection*vec4(p,1);return q.xy/q.w*.5+.5;}
vec3 unproject(vec2 uv,float depth){vec4 p=uInvProjection*vec4(uv*2.0-1.0,depth*2.0-1.0,1);return p.xyz/p.w;}
vec3 studio(vec3 d){
    vec3 c=vec3(.12+.22*max(d.y,0.0));
    c+=vec3(2.0)*exp(-pow(length(d-normalize(vec3(.3,.85,.4))),2.0)*24.0);
    c+=vec3(.95)*exp(-pow(length(d-normalize(vec3(-.7,.4,-.3))),2.0)*32.0);
    c+=vec3(.65)*exp(-pow(length(d-normalize(vec3(.4,.2,-.9))),2.0)*45.0);
    return c;
}
void main(){
    vec2 uv=gl_FragCoord.xy/uGlassSize;
    vec3 front=(uView*vec4(vFragPos,1)).xyz;
    vec3 N=normalize(mat3(uView)*vNormal),I=normalize(front);
    if(dot(N,I)>0.0)N=-N;
    float ior=uGlassIOR;vec3 inside=refract(I,N,1.0/ior);
    float backZ=unproject(uv,texture(uExitDepth,uv).r).z;
    float distanceIn=clamp((backZ-front.z)/min(inside.z,-.05),.001,4.0);
    vec3 exitP=front+inside*distanceIn;vec2 exitUV=projectUV(exitP);
    for(int k=0;k<2;++k){
        if(any(lessThan(exitUV,vec2(.001)))||any(greaterThan(exitUV,vec2(.999))))break;
        float dz=texture(uExitDepth,exitUV).r;
        if(dz<.00001)break;
        float z=unproject(exitUV,dz).z;
        distanceIn=clamp((z-front.z)/min(inside.z,-.05),.001,4.0);
        exitP=front+inside*distanceIn;exitUV=projectUV(exitP);
    }
    vec3 exitN=normalize(mat3(uView)*(texture(uExitNormal,clamp(exitUV,.001,.999)).xyz*2.0-1.0));
    if(dot(exitN,inside)<0.0)exitN=-exitN;
    vec3 outgoing=refract(inside,-exitN,ior);
    bool tir=dot(outgoing,outgoing)<.01;
    if(tir)outgoing=reflect(inside,-exitN);
    // March the refracted ray against captured scene depth; it includes
    // already-rendered glass pieces because objects draw back-to-front.
    vec2 hitUV=exitUV;float travel=.02;
    for(int k=0;k<24;++k){
        vec3 p=exitP+outgoing*travel;vec2 q=projectUV(p);
        if(p.z>-.1||any(lessThan(q,vec2(.002)))||any(greaterThan(q,vec2(.998))))break;
        hitUV=q;float depth=texture(uSceneDepth,q).r;
        if(depth<.999999&&p.z<=unproject(q,depth).z+.025)break;
        travel+=.025+travel*.22;
    }
    float edge=smoothstep(0.0,.03,min(min(hitUV.x,hitUV.y),min(1.0-hitUV.x,1.0-hitUV.y)));
    vec3 transmitted=pow(texture(uSceneColor,clamp(mix(uv,hitUV,edge),.001,.999)).rgb,vec3(2.2));
    // Beer-Lambert: nearly clear white, neutrally absorbing smoky black.
    float absorption=uGlassWhite!=0?.28:3.8;
    float attenuation=exp(-absorption*distanceIn);
    vec3 body=uGlassWhite!=0?vec3(.72):vec3(.009);
    transmitted=transmitted*attenuation+body*(1.0-attenuation)*(uGlassWhite!=0?.45:.25);
    float f0=pow((ior-1.0)/(ior+1.0),2.0);
    float fresnel=f0+(1.0-f0)*pow(1.0-clamp(dot(-I,N),0.0,1.0),5.0);
    if(tir)fresnel=max(fresnel,.8);
    vec3 reflection=studio(transpose(mat3(uView))*reflect(I,N));
    vec3 result=mix(transmitted,reflection,clamp(fresnel,0.0,.98));
    FragColor=vec4(pow(clamp(result,0.0,1.0),vec3(1.0/2.2)),1.0);
}
)";
