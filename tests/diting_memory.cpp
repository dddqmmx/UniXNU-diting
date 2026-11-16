// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "DitingMemoryPolicy.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
using namespace DitingMemory;
static void put16(std::vector<uint8_t> &db,size_t at,uint16_t v){db.at(at)=uint8_t(v);db.at(at+1)=uint8_t(v>>8);}
static void put32(std::vector<uint8_t> &db,size_t at,uint32_t v){put16(db,at,uint16_t(v));put16(db,at+2,uint16_t(v>>16));}
static std::vector<uint8_t> database(){
 std::vector<uint8_t> db(0x20000);
 put32(db,0,1);put32(db,4,0x0c0330db);put16(db,8,5);put16(db,10,0);put16(db,12,64);put16(db,14,2);
 for(unsigned i=0;i<2;++i){
  const size_t at=144+24*i;std::memcpy(db.data()+at,i?"SH5":"MC3",3);
  put32(db,at+16,0x50000+4*i);put16(db,at+20,8);put16(db,at+22,uint16_t(8*i));
  put32(db,208+8*i,1000000);put16(db,212+8*i,1);db[214+8*i]=uint8_t(i+1);
 }
 return db;
}
struct Cache {
 std::array<std::map<unsigned,uint32_t>,Banks> bank;
 unsigned writes=0;uint64_t us=0;bool timeout=false,badReadback=false;
 Cache(){for(auto &b:bank){b[statusReg(11)]=Inactive;b[statusReg(12)]=Inactive;for(unsigned r:{WriteScid,WriteCacheable,DisableCapacity,Retain})b[r]=0x80001000;}}
 static uint32_t read(void *p,unsigned b,unsigned r){auto &c=*static_cast<Cache *>(p);return c.bank[b][r];}
 static uint32_t broadcast(void *p,unsigned r){return read(p,0,r);}
 static void write(void *p,unsigned r,uint32_t v){
  auto &c=*static_cast<Cache *>(p);++c.writes;
  for(unsigned i=0;i<Banks;++i){
   if(!(c.badReadback && i==3 && r==attr0Reg(12)))c.bank[i][r]=v;
   if(!c.timeout && (r==11*0x1000 || r==12*0x1000) && v==2)c.bank[i][r+4]=Active;
  }
 }
 static uint64_t now(void *p){return static_cast<Cache *>(p)->us;}
 static void delay(void *p,unsigned us){static_cast<Cache *>(p)->us+=us;}
 CacheIo io(){return {this,read,broadcast,write,now,delay};}
};
int main(){
 unsigned cases=0;
 {
  auto db=database();Bcm ddr={},cache={};uint32_t vote=0;
  assert(readBcm(db.data(),db.size(),"MC3",ddr));assert(readBcm(db.data(),db.size(),"SH5",cache));
  assert(frequencyVote(ddr,DdrMaxKhz,DdrMaxKhz,vote) && vote==0x60000c7c);
  assert(frequencyVote(cache,LlccMaxKhz,LlccMaxKhz,vote) && vote==0x6000042a);++cases;
  assert(!frequencyVote(ddr,DdrMaxKhz+1,DdrMaxKhz,vote));ddr.width=65535;assert(!frequencyVote(ddr,DdrMaxKhz,DdrMaxKhz,vote));++cases;
 }
 for(unsigned corrupt=0;corrupt<5;++corrupt){
  auto db=database();Bcm out={};
  if(corrupt==0)put16(db,164,7); // wrong aux length
  if(corrupt==1)put16(db,166,65535); // out-of-range aux
  if(corrupt==2)put32(db,208,999); // unsupported command-db unit
  if(corrupt==3)std::memcpy(db.data()+168,"MC3",3); // duplicate identity
  if(corrupt==4)put16(db,12,0); // aux overlaps entry table
  assert(!readBcm(db.data(),db.size(),"MC3",out));++cases;
 }
 {
  Cache c;auto io=c.io();std::array<uint32_t,4> before={};const unsigned regs[]={WriteScid,WriteCacheable,DisableCapacity,Retain};
  for(unsigned i=0;i<4;++i)before[i]=c.bank[0][regs[i]];
  assert(configureSlice(io,11,512,false)==CacheResult::Ok);
  assert(configureSlice(io,12,2048,true)==CacheResult::Ok);
  for(unsigned b=0;b<Banks;++b){
   assert(c.bank[b][attr0Reg(12)]==0xffff0000 && c.bank[b][attr1Reg(12)]==0x20000018);
   assert(c.bank[b][attr1Reg(11)]==0x08000018);
   for(unsigned i=0;i<4;++i)assert((c.bank[b][regs[i]]&~((1u<<11)|(1u<<12)))==(before[i]&~((1u<<11)|(1u<<12))));
  }
  assert(gpuScache0()==0x3100 && gpuScache1()==0x18c6318c);++cases;
  const unsigned beforeWrites=c.writes;assert(configureSlice(io,12,2048,true)==CacheResult::Ok);assert(c.writes==beforeWrites);++cases;
 }
 {
  Cache c;for(auto &b:c.bank)b[statusReg(12)]=Active;
  assert(configureSlice(c.io(),12,2048,true)==CacheResult::ForeignActive && c.writes==0);++cases;
 }
 {
  Cache c;for(auto &b:c.bank)b[statusReg(12)]=0;
  assert(configureSlice(c.io(),12,2048,true)==CacheResult::Unavailable && c.writes==0);++cases;
 }
 {
  Cache c;c.timeout=true;assert(configureSlice(c.io(),12,2048,true)==CacheResult::Timeout && c.us<=100);++cases;
 }
 {
  Cache c;c.badReadback=true;assert(configureSlice(c.io(),12,2048,true)==CacheResult::Readback);assert(c.bank[3][statusReg(12)]==Inactive);++cases;
 }
 std::printf("PASS %u memory policy cases\n",cases);
}
