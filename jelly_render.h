// Renderer-private implementation, included after g_pieces is declared.
// All allocations are reused; no GL resources are owned by copied game states.
struct JellyTarget { GLuint fbo=0,color=0,depth=0; };
static JellyTarget jelly_main,jelly_scene,jelly_exit;
static int jelly_width=0,jelly_height=0;
static GLuint jelly_nodes=0,jelly_glass_program=0,jelly_exit_program=0;
static float jelly_ior=1.36f;
void renderer_dbg_jelly_ior(float ior){jelly_ior=std::clamp(ior,1.f,1.6f);}
struct JellyDraw { int type;Mat4 model;bool white;const JellyMotion* motion; };
static std::vector<JellyDraw> jelly_draws;

static void jelly_uniform(GLuint program,const JellyMotion* motion,bool enabled) {
    // Every linked sampler gets a distinct, complete binding even when
    // deformation is disabled (WebGL validates statically active samplers).
    glUniform1i(glGetUniformLocation(program,"uJellyNodes"),8);
    if(!jelly_nodes) {
        glGenTextures(1,&jelly_nodes);glActiveTexture(GL_TEXTURE8);glBindTexture(GL_TEXTURE_2D,jelly_nodes);
        std::array<float,jelly::NN*4> zero{};
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,jelly::NN,1,0,GL_RGBA,GL_FLOAT,zero.data());
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }
    glActiveTexture(GL_TEXTURE8);glBindTexture(GL_TEXTURE_2D,jelly_nodes);
    bool deform=enabled&&motion&&motion->awake&&!motion->x.empty();
    glUniform4f(glGetUniformLocation(program,"uJelly"),0,0,0,deform?1.f:0.f);
    if(deform) {
        const auto& c=jelly::cage(motion->type);std::array<float,jelly::NN*4> data{};
        for(int i=0;i<jelly::NN;++i)for(int k=0;k<3;++k)data[4*i+k]=motion->x[i][k]-c.rest[i][k];
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,jelly::NN,1,GL_RGBA,GL_FLOAT,data.data());
        glUniform3f(glGetUniformLocation(program,"uJellyLo"),c.lo.x,c.lo.y,c.lo.z);
        glUniform3f(glGetUniformLocation(program,"uJellyInvSpacing"),1/c.spacing.x,1/c.spacing.y,1/c.spacing.z);
    }
    glActiveTexture(GL_TEXTURE0);
}
static void ensure_jelly_targets(int w,int h) {
    if(w==jelly_width&&h==jelly_height&&jelly_main.fbo)return;
    GLint previous;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&previous);
    glActiveTexture(GL_TEXTURE0);
    for(auto* t:{&jelly_main,&jelly_scene,&jelly_exit}) {
        if(!t->fbo){glGenFramebuffers(1,&t->fbo);glGenTextures(1,&t->color);glGenTextures(1,&t->depth);}
        glBindTexture(GL_TEXTURE_2D,t->color);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D,t->depth);glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT24,w,h,0,GL_DEPTH_COMPONENT,GL_UNSIGNED_INT,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER,t->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t->color,0);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,t->depth,0);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("Jelly transmission framebuffer incomplete");
    }
    glBindTexture(GL_TEXTURE_2D,0);glBindFramebuffer(GL_FRAMEBUFFER,previous);
    jelly_width=w;jelly_height=h;
}
static void jelly_copy_scene(GLuint source,int x,int y,int w,int h) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER,source);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,jelly_scene.fbo);
    glBlitFramebuffer(x,y,x+w,y+h,x,y,x+w,y+h,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER,source);
}
[[maybe_unused]] static void jelly_present(GLuint destination,int x,int y,int w,int h) {
    // WebGL forbids blitting a single-sample source into the multisampled
    // browser canvas. A textured fullscreen draw works on either format.
    static GLuint program=0,vao=0,vbo=0;
    if(!program) {
        program=create_program(splat_blit_vs_src,splat_blit_fs_src);
        float quad[]={-1,-1,1,-1,-1,1,-1,1,1,-1,1,1};
        glGenVertexArrays(1,&vao);glGenBuffers(1,&vbo);glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);glEnableVertexAttribArray(0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER,destination);
    glViewport(x,y,w,h);glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);
    glUseProgram(program);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,jelly_main.color);
    glUniform1i(glGetUniformLocation(program,"uTex"),0);glBindVertexArray(vao);glDrawArrays(GL_TRIANGLES,0,6);glBindVertexArray(0);
}
static void jelly_flush(const Mat4& view,const Mat4& projection,int w,int h) {
    if(jelly_draws.empty())return;
    GLint source,previous_program;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&source);glGetIntegerv(GL_CURRENT_PROGRAM,&previous_program);
    ensure_jelly_targets(w,h);
    if(!jelly_glass_program) {
        jelly_glass_program=create_program(vertex_shader_src,jelly_glass_fs_src);
        jelly_exit_program=create_program(vertex_shader_src,jelly_exit_fs_src);
        if(!jelly_glass_program||!jelly_exit_program)throw std::runtime_error("Jelly shader compilation failed");
    }
    bool blend=glIsEnabled(GL_BLEND),cull=glIsEnabled(GL_CULL_FACE),depth=glIsEnabled(GL_DEPTH_TEST);
    GLint old_cull,old_depth;GLboolean old_mask;
    glGetIntegerv(GL_CULL_FACE_MODE,&old_cull);glGetIntegerv(GL_DEPTH_FUNC,&old_depth);glGetBooleanv(GL_DEPTH_WRITEMASK,&old_mask);
    GLfloat old_clear[4];glGetFloatv(GL_COLOR_CLEAR_VALUE,old_clear);
    glDisable(GL_BLEND);glEnable(GL_DEPTH_TEST);glEnable(GL_CULL_FACE);glDepthMask(GL_TRUE);
    Mat4 vp=mat4_multiply(projection,view),inv=mat4_inverse(projection);
    std::stable_sort(jelly_draws.begin(),jelly_draws.end(),[&](const JellyDraw& a,const JellyDraw& b){
        return mat4_mul_vec4(view,{a.model.m[12],a.model.m[13],a.model.m[14],1}).z<mat4_mul_vec4(view,{b.model.m[12],b.model.m[13],b.model.m[14],1}).z;
    });
    jelly_copy_scene(source,0,0,w,h);
    for(const auto& d:jelly_draws) {
        // Conservative projected cage bounds limit exit clears and resolves.
        float minx=float(w),miny=float(h),maxx=0,maxy=0;bool clipped=false;
        const auto& c=jelly::cage(d.type);Mat4 mvp=mat4_multiply(vp,d.model);
        for(int i=0;i<jelly::NN;++i) {
            auto p=d.motion&&d.motion->awake&&!d.motion->x.empty()?d.motion->x[i]:c.rest[i];
            Vec4 q=mat4_mul_vec4(mvp,{p.x,p.y,p.z,1});if(q.w<=.1f){clipped=true;break;}
            float x=(q.x/q.w*.5f+.5f)*w,y=(q.y/q.w*.5f+.5f)*h;
            minx=std::min(minx,x);maxx=std::max(maxx,x);miny=std::min(miny,y);maxy=std::max(maxy,y);
        }
        int x=clipped?0:std::clamp(int(std::floor(minx))-3,0,w),y=clipped?0:std::clamp(int(std::floor(miny))-3,0,h);
        int rw=clipped?w:std::clamp(int(std::ceil(maxx))+3,0,w)-x,rh=clipped?h:std::clamp(int(std::ceil(maxy))+3,0,h)-y;
        if(rw<=0||rh<=0)continue;
        // Unbind attached textures before writing, avoiding WebGL feedback.
        for(int unit=0;unit<4;++unit){glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_2D,0);}
        glBindFramebuffer(GL_FRAMEBUFFER,jelly_exit.fbo);glViewport(0,0,w,h);
        glDisable(GL_SCISSOR_TEST);glClearColor(.5f,.5f,.5f,0);glClearDepthf(0);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);glScissor(x,y,rw,rh);glDepthFunc(GL_GREATER);glCullFace(GL_FRONT);
        float nm[9];mat4_normal_matrix(d.model,nm);
        for(GLuint prog:{jelly_exit_program,jelly_glass_program}) {
            glUseProgram(prog);
            glUniformMatrix4fv(glGetUniformLocation(prog,"uView"),1,GL_FALSE,view.m);
            glUniformMatrix4fv(glGetUniformLocation(prog,"uProjection"),1,GL_FALSE,projection.m);
            glUniformMatrix4fv(glGetUniformLocation(prog,"uModel"),1,GL_FALSE,d.model.m);
            glUniformMatrix3fv(glGetUniformLocation(prog,"uNormalMat"),1,GL_FALSE,nm);
            jelly_uniform(prog,d.motion,true);
            if(prog==jelly_glass_program) {
                glBindFramebuffer(GL_FRAMEBUFFER,source);glDepthFunc(GL_LESS);glCullFace(GL_BACK);
                const GLuint tex[4]={jelly_scene.color,jelly_scene.depth,jelly_exit.color,jelly_exit.depth};
                const char* names[4]={"uSceneColor","uSceneDepth","uExitNormal","uExitDepth"};
                for(int i=0;i<4;++i){glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,tex[i]);glUniform1i(glGetUniformLocation(prog,names[i]),i);}
                glUniformMatrix4fv(glGetUniformLocation(prog,"uInvProjection"),1,GL_FALSE,inv.m);
                glUniform2f(glGetUniformLocation(prog,"uGlassSize"),float(w),float(h));
                glUniform1i(glGetUniformLocation(prog,"uGlassWhite"),d.white?1:0);
                glUniform1f(glGetUniformLocation(prog,"uGlassIOR"),jelly_ior);
            }
            glBindVertexArray(g_pieces[d.type].vao);glDrawArrays(GL_TRIANGLES,0,g_pieces[d.type].num_vertices);
        }
        glDisable(GL_SCISSOR_TEST);
        jelly_copy_scene(source,x,y,rw,rh);
    }
    jelly_draws.clear();glBindVertexArray(0);glUseProgram(previous_program);
    glClearDepthf(1);glClearColor(old_clear[0],old_clear[1],old_clear[2],old_clear[3]);
    glDepthFunc(old_depth);glDepthMask(old_mask);glCullFace(old_cull);
    if(!depth)glDisable(GL_DEPTH_TEST);
    if(!cull)glDisable(GL_CULL_FACE);
    if(blend)glEnable(GL_BLEND);
    // Subsequent board overlays expect the normal shadow texture on unit 0.
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,g_shadow_tex);
}
