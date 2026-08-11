#include <GLES3/gl3.h>
#include <jni.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {
struct Vertex { float x,y,r,g,b; };
GLuint program=0, vao=0, vbo=0;
int screen_w=1, screen_h=1;
bool menu_open=false, search_focused=false;
std::string page="INICIO";
std::vector<Vertex> vertices;
constexpr const char* vs=R"(#version 300 es
layout(location=0) in vec2 position; layout(location=1) in vec3 color; out vec3 c;
void main(){c=color;gl_Position=vec4(position,0.0,1.0);})";
constexpr const char* fs=R"(#version 300 es
precision mediump float; in vec3 c; out vec4 out_color; void main(){out_color=vec4(c,1.0);})";
GLuint compile(GLenum t,const char* s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,nullptr);glCompileShader(x);return x;}
float nx(float x){return x/screen_w*2.f-1.f;} float ny(float y){return 1.f-y/screen_h*2.f;}
void rect(float x,float y,float w,float h,float r,float g,float b){
  float l=nx(x), rr=nx(x+w), t=ny(y), bb=ny(y+h); Vertex a{l,t,r,g,b},c{rr,t,r,g,b},d{rr,bb,r,g,b},e{l,bb,r,g,b};
  vertices.insert(vertices.end(),{a,e,d,a,d,c});
}
// Fuente geométrica 5x7: cada píxel del carácter es un pequeño quad enviado al GPU.
std::array<uint8_t,7> glyph(char c){
 switch(c){
 case 'A':return {14,17,17,31,17,17,17};case 'B':return {30,17,17,30,17,17,30};case 'C':return {15,16,16,16,16,16,15};case 'D':return {30,17,17,17,17,17,30};case 'E':return {31,16,16,30,16,16,31};case 'F':return {31,16,16,30,16,16,16};case 'G':return {15,16,16,23,17,17,15};case 'H':return {17,17,17,31,17,17,17};case 'I':return {31,4,4,4,4,4,31};case 'J':return {7,2,2,2,2,18,12};case 'K':return {17,18,20,24,20,18,17};case 'L':return {16,16,16,16,16,16,31};case 'M':return {17,27,21,21,17,17,17};case 'N':return {17,25,21,19,17,17,17};case 'O':return {14,17,17,17,17,17,14};case 'P':return {30,17,17,30,16,16,16};case 'Q':return {14,17,17,17,21,18,13};case 'R':return {30,17,17,30,20,18,17};case 'S':return {15,16,16,14,1,1,30};case 'T':return {31,4,4,4,4,4,4};case 'U':return {17,17,17,17,17,17,14};case 'V':return {17,17,17,17,17,10,4};case 'W':return {17,17,17,21,21,21,10};case 'X':return {17,17,10,4,10,17,17};case 'Y':return {17,17,10,4,4,4,4};case 'Z':return {31,1,2,4,8,16,31};case '0':return {14,17,19,21,25,17,14};case '1':return {4,12,4,4,4,4,14};case '2':return {14,17,1,2,4,8,31};case '3':return {30,1,1,14,1,1,30};case '4':return {2,6,10,18,31,2,2};case '5':return {31,16,16,30,1,1,30};case '6':return {14,16,16,30,17,17,14};case '7':return {31,1,2,4,8,8,8};case '8':return {14,17,17,14,17,17,14};case '9':return {14,17,17,15,1,1,14};case '.':return {0,0,0,0,0,6,6};case '-':return {0,0,0,31,0,0,0};default:return {0,0,0,0,0,0,0}; }
}
void text(const std::string& s,float x,float y,float scale,float r,float g,float b){
 for(char raw:s){char c=(raw>='a'&&raw<='z')?raw-32:raw;auto rows=glyph(c);for(int row=0;row<7;++row)for(int col=0;col<5;++col)if(rows[row]&(1<<(4-col)))rect(x+col*scale,y+row*scale,scale,scale,r,g,b);x+=6*scale;}
}
void draw_ui(){
 // Fondo, barra superior y navegacion inferior
 rect(0,0,(float)screen_w,(float)screen_h,.07f,.08f,.10f);
 rect(0,0,(float)screen_w,70,.10f,.12f,.15f);
 text("NUBY",24,23,4,.92f,.95f,1.f);
 text("<",screen_w-138,25,4,.68f,.74f,.82f); text("O",screen_w-96,25,4,.68f,.74f,.82f); text("MENU",screen_w-63,25,3,.68f,.74f,.82f);
 // Inicio: logo, búsqueda y boton de envio reales.
 if(page=="INICIO"){
   text("NUBY",screen_w/2-48,screen_h/2-118,7,.18f,.55f,1.f);
   float bx=24,by=screen_h/2-42,bw=screen_w-48;
   rect(bx,by,bw,58, search_focused ? .13f : .11f, search_focused ? .31f : .17f, search_focused ? .36f : .21f);
   text(search_focused?"ESCRIBE BUSQUEDA":"BUSCAR VIDEOJUEGOS TECNOLOGIA",bx+16,by+21,2.1f,.78f,.82f,.88f);
   rect(bx+bw-58,by+7,50,44,.10f,.42f,.88f);text(">",bx+bw-40,by+18,4,1,1,1);
 } else { text(page,30,106,5,.92f,.95f,1.f); text("PANEL NATIVO DE NUBY",30,150,2.5f,.60f,.67f,.75f); }
 rect(0,screen_h-80,(float)screen_w,80,.10f,.12f,.15f);
 text("INICIO",26,screen_h-47,2.5f,.55f,.65f,.77f);text("HISTORIAL",screen_w*.30f,screen_h-47,2.5f,.55f,.65f,.77f);text("AJUSTES",screen_w*.68f,screen_h-47,2.5f,.55f,.65f,.77f);
 if(menu_open){
   float x=screen_w-235,y=76;rect(x,y,215,270,.96f,.97f,1.f);
   text("MENU",x+18,y+20,3,.12f,.14f,.18f);const std::array<std::string,5> items{{"HISTORIAL","AJUSTES","DESCARGAS","BORRAR DATOS","ACERCA"}};
   for(int i=0;i<5;i++){float iy=y+57+i*40;rect(x+8,iy,199,34,.91f,.94f,.99f);text(items[i],x+18,iy+11,2.2f,.13f,.30f,.62f);}
 }
}
void flush(){glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,vertices.size()*sizeof(Vertex),vertices.data(),GL_DYNAMIC_DRAW);glUseProgram(program);glBindVertexArray(vao);glDrawArrays(GL_TRIANGLES,0,(GLsizei)vertices.size());}
}
extern "C" JNIEXPORT void JNICALL Java_com_nuby_browser_MainActivity_nativeCreate(JNIEnv*,jclass){GLuint a=compile(GL_VERTEX_SHADER,vs),b=compile(GL_FRAGMENT_SHADER,fs);program=glCreateProgram();glAttachShader(program,a);glAttachShader(program,b);glLinkProgram(program);glDeleteShader(a);glDeleteShader(b);glGenVertexArrays(1,&vao);glGenBuffers(1,&vbo);glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,vbo);glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(Vertex),(void*)0);glEnableVertexAttribArray(1);glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),(void*)(2*sizeof(float)));}
extern "C" JNIEXPORT void JNICALL Java_com_nuby_browser_MainActivity_nativeResize(JNIEnv*,jclass,jint w,jint h){screen_w=w;screen_h=h;glViewport(0,0,w,h);}
extern "C" JNIEXPORT void JNICALL Java_com_nuby_browser_MainActivity_nativeFrame(JNIEnv*,jclass){vertices.clear();draw_ui();glClearColor(.07f,.08f,.10f,1);glClear(GL_COLOR_BUFFER_BIT);flush();}
extern "C" JNIEXPORT void JNICALL Java_com_nuby_browser_MainActivity_nativeTouch(JNIEnv*,jclass,jint action,jfloat x,jfloat y){if(action!=1)return; if(y<70&&x>screen_w-90){menu_open=!menu_open;return;}if(page=="INICIO"&&y>screen_h/2-50&&y<screen_h/2+30){search_focused=true;return;}if(menu_open&&x>screen_w-235&&y>130&&y<346){int item=(int)((y-133)/40);static const std::array<std::string,5> pages{{"HISTORIAL","AJUSTES","DESCARGAS","INICIO","ACERCA"}};page=pages[std::min(4,std::max(0,item))];menu_open=false;return;}if(y>screen_h-80){page=x<screen_w*.25f?"INICIO":(x<screen_w*.62f?"HISTORIAL":"AJUSTES");}}
