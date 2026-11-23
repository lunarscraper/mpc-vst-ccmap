/* Offline test of the EC4 display (ec4_text.h + the mapper): g++ -std=c++17 -I. ec4_test.cpp && ./a.out */
#include "ccmap_core.h"
#include <cstdio>
using namespace ccmap;
static int fails=0;
#define CHECK(c,...) do{ if(!(c)){ fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } }while(0)
static std::string hex(const std::vector<uint8_t>&v){std::string s;char b[4];for(auto x:v){std::snprintf(b,4,"%02X ",x);s+=b;}return s;}
int main(){
  CHECK(ec4::abbrev("Cutoff")=="Ctff","cutoff '%s'",ec4::abbrev("Cutoff").c_str());
  CHECK(ec4::abbrev("Echo Time")=="EcTi","echo '%s'",ec4::abbrev("Echo Time").c_str());
  CHECK(ec4::abbrev("Lane 12")=="Ln12","lane '%s'",ec4::abbrev("Lane 12").c_str());
  CHECK(ec4::abbrev("VOL")=="VOL ","vol");
  CHECK(ec4::abbrev("Größe")=="Grss" || ec4::abbrev("Größe").size()==4,"umlaut '%s'",ec4::abbrev("Größe").c_str());
  CHECK(ec4::abbrev("")=="    ","empty");
  CHECK(ec4::abbrev("Steps3")=="Stp3","steps3 '%s'",ec4::abbrev("Steps3").c_str());
  auto m=ec4::name_msg(2,"Ctff");
  CHECK(m.size()==26,"len %zu",m.size());
  std::printf("cell 3 'Ctff': %s\n",hex(m).c_str());
  CHECK(hex(m)=="F0 00 00 00 4E 2C 1B 4E 22 10 4A 20 18 4D 24 13 4D 27 14 4D 26 16 4D 26 16 F7 ","bytes");
  // reports
  uint8_t g[]={0xF0,0,0,0,0x4E,0x24,0x13,0xF7}; auto r=ec4::parse(g,8); CHECK(r.ok&&r.group==3,"group");
  uint8_t sp[]={0xF0,0,0,0,0x4E,0x2A,0x15,0x4E,0x2E,0x11,0xF7}; r=ec4::parse(sp,11); CHECK(r.shift_push==5,"push");
  uint8_t spu[]={0xF0,0,0,0,0x4E,0x2A,0x15,0x4E,0x2E,0x10,0xF7}; r=ec4::parse(spu,11); CHECK(r.shift_push==-1,"release");
  ec4::Assembler a; uint8_t p1[]={0xF0,0,0,0,0x4E}, p2[]={0x28,0x12,0xF7};
  CHECK(!a.feed(p1,5)&&a.feed(p2,3),"assemble"); r=ec4::parse(a.buf.data(),(int)a.buf.size()); CHECK(r.setup==2,"setup");
  // mapper
  Mapper mp; mp.ec4_report(ec4::parse(g,8));
  mp.sel=0; mp.learn=true; mp.input(DEV_EC4,0xB0,1,20); mp.set_name(0,"Cutoff");
  mp.sel=1; mp.learn=true; mp.input(DEV_EC4,0xB0,18,20);  // auto cell 2 (CC18), unnamed -> C18
  mp.sel=2; mp.learn=true; mp.clock=100; mp.input(DEV_EC4,0xB0,74,20); mp.set_name(2,"Resonance");
  mp.clock=105; mp.ec4_report(ec4::parse(sp,11)); // shift+push encoder 6
  CHECK(mp.map[2].cell==6 && mp.map[2].grp==4,"assign cell %d grp %d",mp.map[2].cell,mp.map[2].grp);
  std::string c[16]; mp.ec4_screen(c);
  CHECK(c[0]=="Ctff"&&c[1]=="C18 "&&c[5]=="Rsnn"&&c[3]=="    ","screen '%s' '%s' '%s'",c[0].c_str(),c[1].c_str(),c[5].c_str());
  // other group: hidden
  uint8_t g5[]={0xF0,0,0,0,0x4E,0x24,0x15,0xF7}; mp.ec4_report(ec4::parse(g5,8)); mp.ec4_screen(c);
  CHECK(c[0]=="    ","group filter '%s'",c[0].c_str());
  // state roundtrip
  std::string st=mp.state(); Mapper m2; m2.apply(st);
  CHECK(m2.map[2].cell==6&&m2.map[2].grp==4&&!std::strcmp(m2.map[2].name,"Resonance"),"roundtrip %s",st.c_str());
  // late shift-push ignored
  mp.sel=3; mp.learn=true; mp.clock=200; mp.input(DEV_EC4,0xB0,5,1); mp.clock=230; mp.ec4_report(ec4::parse(sp,11));
  CHECK(mp.map[3].cell==0,"late push");
  std::printf(fails?"FAILED %d\n":"PASSED\n",fails); return fails!=0;
}
