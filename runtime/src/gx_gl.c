/* OpenGL 3.3 backend for the GX renderer: draws on the host GPU instead of the software
 * rasterizer in gx_render.c, which still does everything around it (vertex transform, texture
 * decoding, building the per-draw RasterState, EFB copies from the CPU-side EFB).
 *
 * The EFB is an offscreen 640x528 RGBA8 + depth framebuffer, stored top row first (screen y
 * maps straight to GL row y), so glReadPixels of a rectangle gives EFB rows as they are. The
 * TEV runs in one "uber" fragment shader that interprets the RasterState from uniforms with the
 * same integer arithmetic as tev_shade(), so both renderers agree.
 *
 * Triangles are batched until the state, scissor or a texture changes, or the EFB is read.
 * Everything runs on the GPU (FIFO decoder) thread, where the context is made current. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include "gx_gl.h"

/* ------------------------------------------------------------------ GL entry points */
#define GL_FUNCS(X) \
    X(void, glEnable, (GLenum)) X(void, glDisable, (GLenum)) X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) X(void, glClear, (GLbitfield)) \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) X(void, glClearDepth, (GLdouble)) \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) X(void, glDepthMask, (GLboolean)) \
    X(void, glDepthFunc, (GLenum)) X(void, glBlendFunc, (GLenum, GLenum)) X(void, glBlendEquation, (GLenum)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)) \
    X(void, glPixelStorei, (GLenum, GLint)) X(void, glGenTextures, (GLsizei, GLuint*)) \
    X(void, glDeleteTextures, (GLsizei, const GLuint*)) X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glActiveTexture, (GLenum)) X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
    X(GLenum, glGetError, (void)) X(const GLubyte*, glGetString, (GLenum)) \
    X(void, glGenBuffers, (GLsizei, GLuint*)) X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint*)) X(void, glBindVertexArray, (GLuint)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(void, glCompileShader, (GLuint)) X(void, glGetShaderiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*)) X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void, glUseProgram, (GLuint)) X(GLint, glGetUniformLocation, (GLuint, const GLchar*)) \
    X(void, glUniform1i, (GLint, GLint)) X(void, glUniform1iv, (GLint, GLsizei, const GLint*)) \
    X(void, glUniform4iv, (GLint, GLsizei, const GLint*)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint*)) X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint*)) X(void, glBindRenderbuffer, (GLenum, GLuint)) \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*)) \
    X(void, glBindFragDataLocation, (GLuint, GLuint, const GLchar*))

#define X(ret, name, args) typedef ret (APIENTRY* PFN_##name) args; static PFN_##name p_##name;
GL_FUNCS(X)
#undef X
#define X(ret, name, args) +1
enum { GL_FUNC_COUNT = 0 GL_FUNCS(X) };
#undef X

static int load_gl(void) {
#define X(ret, name, args) \
    if (!(p_##name = (PFN_##name)SDL_GL_GetProcAddress(#name))) { fprintf(stderr, "[gl] missing %s\n", #name); return 0; }
    GL_FUNCS(X)
#undef X
    return 1;
}

/* ------------------------------------------------------------------ shaders */
static const char* vs_src =
    "#version 330 core\n"
    "in vec4 a_pos; in vec4 a_c0; in vec4 a_c1;\n"
    "in vec3 a_t0; in vec3 a_t1; in vec3 a_t2; in vec3 a_t3; in vec3 a_t4; in vec3 a_t5; in vec3 a_t6; in vec3 a_t7;\n"
    "out vec4 v_c0; out vec4 v_c1; out vec3 v_tc[8];\n"
    "void main() {\n"
    "  gl_Position = a_pos; v_c0 = a_c0; v_c1 = a_c1;\n"
    "  v_tc[0] = a_t0; v_tc[1] = a_t1; v_tc[2] = a_t2; v_tc[3] = a_t3;\n"
    "  v_tc[4] = a_t4; v_tc[5] = a_t5; v_tc[6] = a_t6; v_tc[7] = a_t7;\n"
    "}\n";

/* Mirrors tev_shade() / tev_combine() / alpha_test() in gx_render.c. */
static const char* fs_src =
    "#version 330 core\n"
    "in vec4 v_c0; in vec4 v_c1; in vec3 v_tc[8];\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_tex[8];\n"
    "uniform int u_nstages, u_ntex, u_ncol, u_alpha;\n"
    "uniform ivec4 u_regs[4];\n"
    "uniform ivec4 u_st[16];    /* texen, texcoord, ras_sel, texmap */\n"
    "uniform ivec4 u_tsw[16], u_rsw[16], u_konst[16], u_csel[16], u_asel[16];\n"
    "uniform ivec4 u_cop[16], u_aop[16]; /* bias, sub, clamp, scale */\n"
    "uniform ivec4 u_dest[16];  /* color dest, alpha dest */\n"
    "uniform int u_nind, u_icmd[16];\n"
    "uniform ivec4 u_ind[4];    /* texmap (-1: none), texcoord, scale s, scale t */\n"
    "uniform ivec4 u_mtx[6];    /* indirect matrix rows, .w = scale shift */\n"
    "uniform ivec4 u_tcsize[8]; /* texture size per texture coordinate */\n"
    "uniform ivec4 u_mapsize[8];/* texture size per texture map */\n"
    "vec4 sample_map(int m, vec2 uv) {\n"
    "  if (m == 0) return texture(u_tex[0], uv); if (m == 1) return texture(u_tex[1], uv);\n"
    "  if (m == 2) return texture(u_tex[2], uv); if (m == 3) return texture(u_tex[3], uv);\n"
    "  if (m == 4) return texture(u_tex[4], uv); if (m == 5) return texture(u_tex[5], uv);\n"
    "  if (m == 6) return texture(u_tex[6], uv); return texture(u_tex[7], uv);\n"
    "}\n"
    "int c255(int v) { return clamp(v, 0, 255); }\n"
    "int combine(int A, int B, int C, int D, ivec4 op) {\n"
    "  A &= 255; B &= 255; C &= 255; int v;\n"
    "  if (op.x == 3) v = D + (A > B ? C : 0);\n"
    "  else {\n"
    "    int lerp = (A * (256 - (C + (C >> 7))) + B * (C + (C >> 7))) >> 8;\n"
    "    v = (op.y != 0 ? D - lerp : D + lerp) + (op.x == 1 ? 128 : op.x == 2 ? -128 : 0);\n"
    "    v = op.w == 3 ? v / 2 : v << (op.w == 1 ? 1 : op.w == 2 ? 2 : 0);\n"
    "  }\n"
    "  return op.z != 0 ? c255(v) : clamp(v, -1024, 1023);\n"
    "}\n"
    "bool acmp(int a, int ref, int fn) {\n"
    "  if (fn == 0) return false; if (fn == 1) return a < ref; if (fn == 2) return a == ref;\n"
    "  if (fn == 3) return a <= ref; if (fn == 4) return a > ref; if (fn == 5) return a != ref;\n"
    "  if (fn == 6) return a >= ref; return true;\n"
    "}\n"
    "int tcmp(int op, ivec3 a, ivec3 b) {\n"
    "  int x, y;\n"
    "  if (op < 2) { x = a.x; y = b.x; } else if (op < 4) { x = (a.y << 8) | a.x; y = (b.y << 8) | b.x; }\n"
    "  else { x = (a.z << 16) | (a.y << 8) | a.x; y = (b.z << 16) | (b.y << 8) | b.x; }\n"
    "  return (op & 1) != 0 ? int(x == y) : int(x > y);\n"
    "}\n"
    "int tclamp(int v, int c) { return c != 0 ? c255(v) : clamp(v, -1024, 1023); }\n"
    "void main() {\n"
    "  ivec4 ras[2];\n"
    "  ivec3 itex[4]; itex[0] = itex[1] = itex[2] = itex[3] = ivec3(0);\n"
    "  for (int i = 0; i < u_nind; i++) {\n"
    "    ivec4 is = u_ind[i]; if (is.x < 0) continue;\n"
    "    vec3 t = v_tc[is.y]; vec2 uv = (t.z != 0.0 && t.z != 1.0) ? t.xy / t.z : t.xy;\n"
    "    ivec4 raw = ivec4(sample_map(is.x, uv / vec2(float(1 << is.z), float(1 << is.w))) * 255.0 + 0.5);\n"
    "    itex[i] = ivec3(raw.w, raw.z, raw.y);\n"
    "  }\n"
    "  ivec2 tevc = ivec2(0); int alphabump = 0;\n"
    "  vec4 c0 = (0 < u_ncol || u_ncol == 0) ? v_c0 : vec4(0.0);\n"
    "  vec4 c1 = (1 < u_ncol || u_ncol == 0) ? v_c1 : vec4(0.0);\n"
    "  ras[0] = clamp(ivec4(c0 + 0.5), 0, 255); ras[1] = clamp(ivec4(c1 + 0.5), 0, 255);\n"
    "  ivec4 r[4]; r[0] = u_regs[0]; r[1] = u_regs[1]; r[2] = u_regs[2]; r[3] = u_regs[3];\n"
    "  ivec4 tex = ivec4(255);\n"
    "  ivec4 outc = ivec4(0);\n"
    "  for (int s = 0; s < u_nstages; s++) {\n"
    "    ivec4 st = u_st[s];\n"
    "    vec3 t = v_tc[st.y];\n"
    "    vec2 uv = (t.z != 0.0 && t.z != 1.0) ? t.xy / t.z : t.xy;\n"
    "    int ic = u_icmd[s];\n"
    "    if (ic != 0) {\n"
    "      int bt = ic & 3, fmt = (ic >> 2) & 3, bias = (ic >> 4) & 7, bs = (ic >> 7) & 3;\n"
    "      int mid = (ic >> 9) & 15, sw = (ic >> 13) & 7, tw = (ic >> 16) & 7, fb = (ic >> 20) & 1;\n"
    "      ivec2 sz = max(u_tcsize[st.y].xy, ivec2(1));\n"
    "      ivec2 fuv = ivec2(uv * vec2(sz) * 128.0);\n"
    "      ivec2 it = ivec2(0);\n"
    "      if (bt < u_nind && (bs != 0 || mid != 0)) {\n"
    "        int fm = fmt == 0 ? 255 : fmt == 1 ? 31 : fmt == 2 ? 15 : 7;\n"
    "        int am = fmt == 1 ? 224 : fmt == 2 ? 240 : 248;\n"
    "        ivec3 crd = itex[bt] & ivec3(fm);\n"
    "        if (bs != 0) alphabump = itex[bt][bs - 1] & am;\n"
    "        int bv = fmt == 0 ? -128 : 1;\n"
    "        if ((bias & 1) != 0) crd.x += bv; if ((bias & 2) != 0) crd.y += bv; if ((bias & 4) != 0) crd.z += bv;\n"
    "        int m = -1;\n"
    "        if (mid >= 1 && mid <= 3) { m = mid - 1; it = ivec2(\n"
    "            (u_mtx[m * 2].x * crd.x + u_mtx[m * 2].y * crd.y + u_mtx[m * 2].z * crd.z) >> 3,\n"
    "            (u_mtx[m * 2 + 1].x * crd.x + u_mtx[m * 2 + 1].y * crd.y + u_mtx[m * 2 + 1].z * crd.z) >> 3); }\n"
    "        else if (mid >= 5 && mid <= 7) { m = mid - 5; it = (fuv * crd.x) >> 8; }\n"
    "        else if (mid >= 9 && mid <= 11) { m = mid - 9; it = (fuv * crd.y) >> 8; }\n"
    "        if (m >= 0) { int sh = u_mtx[m * 2].w; it = sh >= 0 ? it * (1 << sh) : it >> (-sh); }\n"
    "      }\n"
    "      int wsu = sw == 1 ? 256 : sw == 2 ? 128 : sw == 3 ? 64 : sw == 4 ? 32 : sw == 5 ? 16 : 0;\n"
    "      int wsv = tw == 1 ? 256 : tw == 2 ? 128 : tw == 3 ? 64 : tw == 4 ? 32 : tw == 5 ? 16 : 0;\n"
    "      ivec2 w = ivec2(sw == 0 ? fuv.x : sw >= 6 ? 0 : fuv.x & ((wsu << 7) - 1),\n"
    "                      tw == 0 ? fuv.y : tw >= 6 ? 0 : fuv.y & ((wsv << 7) - 1));\n"
    "      tevc = fb != 0 ? tevc + w + it : w + it;\n"
    "      tevc = (tevc << 8) >> 8;\n"
    "      uv = vec2(tevc) / (128.0 * vec2(max(u_mapsize[st.w].xy, ivec2(1))));\n"
    "    }\n"
    "    if (st.x != 0) {\n"
    "      ivec4 raw = ivec4(sample_map(st.w, uv) * 255.0 + 0.5);\n"
    "      ivec4 sw = u_tsw[s]; tex = ivec4(raw[sw.x], raw[sw.y], raw[sw.z], raw[sw.w]);\n"
    "    }\n"
    "    ivec4 rasc = ivec4(0);\n"
    "    if (st.z <= 1 || st.z == 5 || st.z == 6) {\n"
    "      ivec4 rc = st.z <= 1 ? ras[st.z] : ivec4(0, 0, 0, st.z == 6 ? alphabump | (alphabump >> 5) : alphabump);\n"
    "      ivec4 sw = u_rsw[s]; rasc = ivec4(rc[sw.x], rc[sw.y], rc[sw.z], rc[sw.w]);\n"
    "    }\n"
    "    ivec3 cin[4]; int ain[4];\n"
    "    ivec4 k = u_konst[s];\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "      int sel = u_csel[s][i];\n"
    "      ivec3 d;\n"
    "      if (sel < 8) { ivec4 x = r[sel >> 1]; d = (sel & 1) != 0 ? ivec3(x.w) : x.xyz; }\n"
    "      else if (sel == 8) d = tex.xyz; else if (sel == 9) d = ivec3(tex.w);\n"
    "      else if (sel == 10) d = rasc.xyz; else if (sel == 11) d = ivec3(rasc.w);\n"
    "      else if (sel == 12) d = ivec3(255); else if (sel == 13) d = ivec3(128);\n"
    "      else if (sel == 14) d = k.xyz; else d = ivec3(0);\n"
    "      cin[i] = d;\n"
    "      int as = u_asel[s][i];\n"
    "      ain[i] = as < 4 ? r[as].w : as == 4 ? tex.w : as == 5 ? rasc.w : as == 6 ? k.w : 0;\n"
    "    }\n"
    "    ivec4 op = u_cop[s];\n"
    "    int cd = u_dest[s].x, ad = u_dest[s].y;\n"
    "    ivec3 res; int ares;\n"
    "    ivec3 a8 = cin[0] & ivec3(255), b8 = cin[1] & ivec3(255);\n"
    "    if (op.x == 3) {\n"
    "      int cop = (op.w << 1) | op.y, all = tcmp(cop, a8, b8);\n"
    "      ivec3 pass = cop < 6 ? ivec3(all) : (cop & 1) != 0 ? ivec3(equal(a8, b8)) : ivec3(greaterThan(a8, b8));\n"
    "      res = ivec3(tclamp(cin[3].x + pass.x * (cin[2].x & 255), op.z), tclamp(cin[3].y + pass.y * (cin[2].y & 255), op.z),\n"
    "                  tclamp(cin[3].z + pass.z * (cin[2].z & 255), op.z));\n"
    "    } else {\n"
    "      res = ivec3(combine(cin[0].x, cin[1].x, cin[2].x, cin[3].x, op),\n"
    "                  combine(cin[0].y, cin[1].y, cin[2].y, cin[3].y, op),\n"
    "                  combine(cin[0].z, cin[1].z, cin[2].z, cin[3].z, op));\n"
    "    }\n"
    "    ivec4 aop = u_aop[s];\n"
    "    if (aop.x == 3) {\n"
    "      int cop = (aop.w << 1) | aop.y;\n"
    "      int pass = cop < 6 ? tcmp(cop, a8, b8) : (cop & 1) != 0 ? int((ain[0] & 255) == (ain[1] & 255)) : int((ain[0] & 255) > (ain[1] & 255));\n"
    "      ares = tclamp(ain[3] + pass * (ain[2] & 255), aop.z);\n"
    "    } else ares = combine(ain[0], ain[1], ain[2], ain[3], aop);\n"
    "    r[cd].xyz = res; r[ad].w = ares;\n"
    "    if (s == u_nstages - 1) outc = ivec4(clamp(r[cd].xyz, 0, 255), c255(r[ad].w));\n"
    "  }\n"
    "  int ac = u_alpha;\n"
    "  bool r0 = acmp(outc.w, ac & 255, (ac >> 16) & 7), r1 = acmp(outc.w, (ac >> 8) & 255, (ac >> 19) & 7);\n"
    "  int lop = (ac >> 22) & 3;\n"
    "  bool pass = lop == 0 ? (r0 && r1) : lop == 1 ? (r0 || r1) : lop == 2 ? (r0 != r1) : (r0 == r1);\n"
    "  if (!pass) discard;\n"
    "  o_color = vec4(outc) / 255.0;\n"
    "}\n";

/* ------------------------------------------------------------------ state */
#define EFB_W 640
#define EFB_H 528
#define VTX_FLOATS 36
#define BATCH_VERTS (3 * 8192)

static int gl_state = -1;  /* -1 not tried, 0 unavailable, 1 active */
static GLuint prog, vao, vbo, fbo, efb_color, efb_depth;
static float batch[BATCH_VERTS * VTX_FLOATS];
static int batch_n;
static const GxGlState* batch_state;
static int batch_sc[4];
static struct {
    GLint nstages, ntex, ncol, alpha, regs, st, tsw, rsw, konst, csel, asel, cop, aop, dest, tex;
    GLint nind, icmd, ind, mtx, tcsize, mapsize;
} u;

/* textures uploaded for decoded TexCache pixel buffers, looked up by that pointer */
typedef struct { const void* px; GLuint id; } GlTex;
#define GLTEX_MAX 2048
static GlTex gltex[GLTEX_MAX];
static int gltex_n;

static GLuint compile(GLenum type, const char* src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[gl] shader compile failed:\n%s\n", log);
        return 0;
    }
    return s;
}

static int init_gl(void) {
    const char* want = getenv("WPR_RENDERER");
    if (want && strcmp(want, "soft") == 0) return 0;
    if (!platform_gl_make_current()) return 0;
    if (!load_gl()) return 0;
    fprintf(stderr, "[gl] %s / %s\n", (const char*)p_glGetString(GL_RENDERER), (const char*)p_glGetString(GL_VERSION));
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;
    prog = p_glCreateProgram();
    p_glAttachShader(prog, vs);
    p_glAttachShader(prog, fs);
    static const char* attrs[] = {"a_pos", "a_c0", "a_c1", "a_t0", "a_t1", "a_t2", "a_t3", "a_t4", "a_t5", "a_t6", "a_t7"};
    for (int i = 0; i < 11; i++) p_glBindAttribLocation(prog, (GLuint)i, attrs[i]);
    p_glBindFragDataLocation(prog, 0, "o_color");
    p_glLinkProgram(prog);
    GLint ok = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        p_glGetProgramInfoLog(prog, sizeof log, NULL, log);
        fprintf(stderr, "[gl] link failed:\n%s\n", log);
        return 0;
    }
    p_glUseProgram(prog);
#define U(f, name) u.f = p_glGetUniformLocation(prog, name)
    U(nstages, "u_nstages"); U(ntex, "u_ntex"); U(ncol, "u_ncol"); U(alpha, "u_alpha"); U(regs, "u_regs");
    U(st, "u_st"); U(tsw, "u_tsw"); U(rsw, "u_rsw"); U(konst, "u_konst"); U(csel, "u_csel"); U(asel, "u_asel");
    U(cop, "u_cop"); U(aop, "u_aop"); U(dest, "u_dest"); U(tex, "u_tex");
    U(nind, "u_nind"); U(icmd, "u_icmd"); U(ind, "u_ind"); U(mtx, "u_mtx"); U(tcsize, "u_tcsize"); U(mapsize, "u_mapsize");
#undef U
    static const GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    p_glUniform1iv(u.tex, 8, units);

    p_glGenVertexArrays(1, &vao);
    p_glBindVertexArray(vao);
    p_glGenBuffers(1, &vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    GLsizei stride = VTX_FLOATS * 4;
    p_glEnableVertexAttribArray(0); p_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, (void*)0);
    p_glEnableVertexAttribArray(1); p_glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)16);
    p_glEnableVertexAttribArray(2); p_glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)32);
    for (int t = 0; t < 8; t++) {
        p_glEnableVertexAttribArray((GLuint)(3 + t));
        p_glVertexAttribPointer((GLuint)(3 + t), 3, GL_FLOAT, GL_FALSE, stride, (void*)(intptr_t)(48 + t * 12));
    }

    p_glGenTextures(1, &efb_color);
    p_glBindTexture(GL_TEXTURE_2D, efb_color);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EFB_W, EFB_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glGenRenderbuffers(1, &efb_depth);
    p_glBindRenderbuffer(GL_RENDERBUFFER, efb_depth);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, EFB_W, EFB_H);
    p_glGenFramebuffers(1, &fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, efb_color, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, efb_depth);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[gl] EFB framebuffer incomplete\n");
        return 0;
    }
    p_glViewport(0, 0, EFB_W, EFB_H);
    p_glClearColor(0, 0, 0, 0);
    p_glClearDepth(0.0);
    p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    GLenum err = p_glGetError();
    if (err) { fprintf(stderr, "[gl] init error %04X\n", err); return 0; }
    fprintf(stderr, "[gl] using the GPU renderer\n");
    return 1;
}

int gx_gl_active(void) {
    if (gl_state < 0) gl_state = init_gl();
    return gl_state;
}

/* ------------------------------------------------------------------ textures */
static GLuint texture_for(const GxGlTexMap* tm) {
    for (int i = 0; i < gltex_n; i++)
        if (gltex[i].px == tm->px) return gltex[i].id;
    if (gltex_n == GLTEX_MAX) {  /* cannot happen while the CPU-side cache is smaller */
        for (int i = 0; i < gltex_n; i++) p_glDeleteTextures(1, &gltex[i].id);
        gltex_n = 0;
    }
    GLuint id;
    p_glGenTextures(1, &id);
    p_glBindTexture(GL_TEXTURE_2D, id);
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, tm->stride);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tm->w, tm->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tm->px);
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gltex[gltex_n].px = tm->px;
    gltex[gltex_n].id = id;
    gltex_n++;
    return id;
}

void gx_gl_forget_texture(const void* px) {
    if (gl_state != 1) return;
    for (int i = 0; i < gltex_n; i++)
        if (gltex[i].px == px) {
            p_glDeleteTextures(1, &gltex[i].id);
            gltex[i] = gltex[--gltex_n];
            return;
        }
}

static GLint gl_wrap(int m) { return m == 1 ? GL_REPEAT : m == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE; }

/* ------------------------------------------------------------------ drawing */
static void apply_state(const GxGlState* s) {
    GLint st[16][4], tsw[16][4], rsw[16][4], konst[16][4], csel[16][4], asel[16][4], cop[16][4], aop[16][4], dest[16][4];
    memset(st, 0, sizeof st);
    int bound = 0;
    for (int i = 0; i < s->nstages; i++) {
        const GxGlStage* g = &s->st[i];
        st[i][0] = g->texen; st[i][1] = g->texcoord; st[i][2] = g->ras_sel; st[i][3] = g->texmap;
        for (int k = 0; k < 4; k++) {
            tsw[i][k] = g->tsw[k]; rsw[i][k] = g->rsw[k]; csel[i][k] = g->csel[k]; asel[i][k] = g->asel[k];
        }
        konst[i][0] = g->konst[0]; konst[i][1] = g->konst[1]; konst[i][2] = g->konst[2]; konst[i][3] = g->kalpha;
        cop[i][0] = g->c_bias; cop[i][1] = g->c_sub; cop[i][2] = g->c_clamp; cop[i][3] = g->c_scale;
        aop[i][0] = g->a_bias; aop[i][1] = g->a_sub; aop[i][2] = g->a_clamp; aop[i][3] = g->a_scale;
        dest[i][0] = g->c_dest; dest[i][1] = g->a_dest; dest[i][2] = dest[i][3] = 0;
        if (g->texen && !(bound & (1 << g->texmap))) {
            bound |= 1 << g->texmap;
            p_glActiveTexture(GL_TEXTURE0 + (GLenum)g->texmap);
            p_glBindTexture(GL_TEXTURE_2D, texture_for(&g->tm));
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(g->tm.wrap_s));
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(g->tm.wrap_t));
            GLint f = g->tm.linear ? GL_LINEAR : GL_NEAREST;
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        }
    }
    GLint icmd[16] = {0}, ind[4][4], mtx[6][4], tcsize[8][4], mapsize[8][4];
    memset(mapsize, 0, sizeof mapsize);
    for (int i = 0; i < s->nstages; i++) {
        icmd[i] = (GLint)s->st[i].ind;
        if (s->st[i].texen) { mapsize[s->st[i].texmap][0] = s->st[i].tm.w; mapsize[s->st[i].texmap][1] = s->st[i].tm.h; }
    }
    for (int i = 0; i < 4; i++) {
        const GxGlIndStage* is = &s->ind[i];
        ind[i][0] = i < s->nind && is->ok ? is->texmap : -1;
        ind[i][1] = is->texcoord; ind[i][2] = is->scale_s; ind[i][3] = is->scale_t;
        if (i < s->nind && is->ok && !(bound & (1 << is->texmap))) {
            bound |= 1 << is->texmap;
            p_glActiveTexture(GL_TEXTURE0 + (GLenum)is->texmap);
            p_glBindTexture(GL_TEXTURE_2D, texture_for(&is->tm));
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(is->tm.wrap_s));
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(is->tm.wrap_t));
            GLint f = is->tm.linear ? GL_LINEAR : GL_NEAREST;
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        }
    }
    for (int m = 0; m < 3; m++)
        for (int r = 0; r < 2; r++)
            for (int k = 0; k < 4; k++) mtx[m * 2 + r][k] = s->indmtx[m][r][k];
    for (int t = 0; t < 8; t++) { tcsize[t][0] = s->texsize[t][0]; tcsize[t][1] = s->texsize[t][1]; tcsize[t][2] = tcsize[t][3] = 0; }
    p_glUniform1i(u.nind, s->nind);
    p_glUniform1iv(u.icmd, 16, icmd);
    p_glUniform4iv(u.ind, 4, &ind[0][0]);
    p_glUniform4iv(u.mtx, 6, &mtx[0][0]);
    p_glUniform4iv(u.tcsize, 8, &tcsize[0][0]);
    p_glUniform4iv(u.mapsize, 8, &mapsize[0][0]);
    p_glActiveTexture(GL_TEXTURE0);
    p_glUniform1i(u.nstages, s->nstages);
    p_glUniform1i(u.ntex, s->ntex);
    p_glUniform1i(u.ncol, s->ncol);
    p_glUniform1i(u.alpha, (GLint)s->alpha_cfg);
    p_glUniform4iv(u.regs, 4, &s->regs[0][0]);
    int n = s->nstages;
    p_glUniform4iv(u.st, n, &st[0][0]); p_glUniform4iv(u.tsw, n, &tsw[0][0]); p_glUniform4iv(u.rsw, n, &rsw[0][0]);
    p_glUniform4iv(u.konst, n, &konst[0][0]); p_glUniform4iv(u.csel, n, &csel[0][0]); p_glUniform4iv(u.asel, n, &asel[0][0]);
    p_glUniform4iv(u.cop, n, &cop[0][0]); p_glUniform4iv(u.aop, n, &aop[0][0]); p_glUniform4iv(u.dest, n, &dest[0][0]);

    uint32_t z = s->zmode;
    if (z & 1) {
        static const GLenum fn[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
        p_glEnable(GL_DEPTH_TEST);
        p_glDepthFunc(fn[(z >> 1) & 7]);
        p_glDepthMask((z & 0x10) ? GL_TRUE : GL_FALSE);
    } else {
        p_glDisable(GL_DEPTH_TEST);
    }
    uint32_t cm = s->cmode;
    if (cm & 1) {
        p_glEnable(GL_BLEND);
        if (cm & 0x800) {
            p_glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
            p_glBlendFunc(GL_ONE, GL_ONE);
        } else {
            static const GLenum src_f[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA,
                                            GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
            static const GLenum dst_f[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
                                            GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
            p_glBlendEquation(GL_FUNC_ADD);
            p_glBlendFunc(src_f[(cm >> 8) & 7], dst_f[(cm >> 5) & 7]);
        }
    } else {
        p_glDisable(GL_BLEND);
    }
    GLboolean cw = (cm & 8) ? GL_TRUE : GL_FALSE;
    p_glColorMask(cw, cw, cw, (cm & 0x10) ? GL_TRUE : GL_FALSE);
}

void gx_gl_flush(void) {
    if (gl_state != 1 || !batch_n) return;
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glUseProgram(prog);
    p_glBindVertexArray(vao);
    apply_state(batch_state);
    p_glEnable(GL_SCISSOR_TEST);
    p_glScissor(batch_sc[0], batch_sc[1], batch_sc[2] - batch_sc[0], batch_sc[3] - batch_sc[1]);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)batch_n * VTX_FLOATS * 4, batch, GL_STREAM_DRAW);
    p_glDrawArrays(GL_TRIANGLES, 0, batch_n);
    batch_n = 0;
}

void gx_gl_triangle(const GxGlState* s, const int sc[4], const float pos[3][4], const float col[3][2][4],
                    const float tc[3][8][3]) {
    if (batch_n && (s != batch_state || memcmp(sc, batch_sc, sizeof batch_sc))) gx_gl_flush();
    if (batch_n + 3 > BATCH_VERTS) gx_gl_flush();
    batch_state = s;
    memcpy(batch_sc, sc, sizeof batch_sc);
    for (int i = 0; i < 3; i++) {
        float* v = &batch[(size_t)(batch_n++) * VTX_FLOATS];
        memcpy(v, pos[i], 16);
        memcpy(v + 4, col[i], 32);
        memcpy(v + 12, tc[i], 96);
    }
}

void gx_gl_read_efb(int x0, int y0, int w, int h, uint8_t (*efb)[EFB_W][4]) {
    gx_gl_flush();
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > EFB_W) w = EFB_W - x0;
    if (y0 + h > EFB_H) h = EFB_H - y0;
    if (w <= 0 || h <= 0) return;
    static uint8_t tmp[EFB_W * EFB_H * 4];
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glReadPixels(x0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (int y = 0; y < h; y++) memcpy(efb[y0 + y][x0], tmp + (size_t)y * w * 4, (size_t)w * 4);
}

void gx_gl_clear(int x0, int y0, int w, int h, const uint8_t rgba[4], float z) {
    gx_gl_flush();
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glEnable(GL_SCISSOR_TEST);
    p_glScissor(x0, y0, w, h);
    p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glDepthMask(GL_TRUE);
    p_glClearColor(rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f);
    p_glClearDepth((double)z);
    p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}
