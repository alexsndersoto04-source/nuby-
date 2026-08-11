#include <GLES3/gl3.h>
#include <android/log.h>
#include <jni.h>
#include <array>

namespace {
GLuint program = 0, vao = 0, vbo = 0;
int screen_w = 1, screen_h = 1;
float pointer_x = 0.f, pointer_y = 0.f;
constexpr const char* vertex_source = R"(#version 300 es
layout(location=0) in vec2 position; layout(location=1) in vec3 color;
out vec3 v_color; void main(){ v_color=color; gl_Position=vec4(position,0.0,1.0); })";
constexpr const char* fragment_source = R"(#version 300 es
precision mediump float; in vec3 v_color; out vec4 out_color;
void main(){ out_color=vec4(v_color,1.0); })";
GLuint shader(GLenum type, const char* src) { GLuint s=glCreateShader(type); glShaderSource(s,1,&src,nullptr); glCompileShader(s); return s; }
void create() {
  GLuint vs=shader(GL_VERTEX_SHADER,vertex_source), fs=shader(GL_FRAGMENT_SHADER,fragment_source);
  program=glCreateProgram(); glAttachShader(program,vs); glAttachShader(program,fs); glLinkProgram(program); glDeleteShader(vs); glDeleteShader(fs);
  constexpr std::array<float,15> vertices{{ 0.f,.68f,.10f,.45f,.95f, -.72f,-.62f,.15f,.75f,.50f, .72f,-.62f,.95f,.35f,.28f }};
  glGenVertexArrays(1,&vao); glGenBuffers(1,&vbo); glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER,vbo);
  glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices.data(),GL_STATIC_DRAW);
  glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,5*sizeof(float),(void*)0);
  glEnableVertexAttribArray(1); glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,5*sizeof(float),(void*)(2*sizeof(float)));
}
}
extern "C" JNIEXPORT void JNICALL Java_com_nuby_browser_MainActivity_nativeCreate(JNIEnv*,jclass){ create(); }
extern "C" JNIEXPORT void JNICALL Java_com_nuby_browser_MainActivity_nativeResize(JNIEnv*,jclass,jint w,jint h){ screen_w=w; screen_h=h; glViewport(0,0,w,h); }
extern "C" JNIEXPORT void JNICALL Java_com_nuby_browser_MainActivity_nativeFrame(JNIEnv*,jclass){
  // El frame se ejecuta en el hilo GL de Android; el swap de doble búfer lo gestiona GLSurfaceView/EGL.
  float pulse=(pointer_x/screen_w)*.08f; glClearColor(.07f+pulse,.08f,.10f,1.f); glClear(GL_COLOR_BUFFER_BIT); glUseProgram(program); glBindVertexArray(vao); glDrawArrays(GL_TRIANGLES,0,3);
}
extern "C" JNIEXPORT void JNICALL Java_com_nuby_browser_MainActivity_nativeTouch(JNIEnv*,jclass,jint,jfloat x,jfloat y){ pointer_x=x; pointer_y=y; }
