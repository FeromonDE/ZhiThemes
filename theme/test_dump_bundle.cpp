#include "../src/source/shellui/include/zt_dump_bundle.hpp"
#include <assert.h>
#include <fstream>
#include <iterator>
#include <vector>
static void put(std::vector<unsigned char>& b,size_t at,uint32_t n) {
  for(unsigned i=0;i<4;++i)b[at+i]=(unsigned char)(n>>(8*i));
}
int main(int argc,char**argv) {
  assert(!zt_dump_plain_bundle(nullptr,100));
  for(size_t off: {size_t(0xb20),size_t(0xb30)}) {
    std::vector<unsigned char> b(off+128,0);
    memcpy(b.data(),"RNPSHEDR",8);put(b,28,(uint32_t)off);
    assert(!zt_dump_plain_bundle(b.data(),b.size()));
    const unsigned char magic[]={0xc6,0x1f,0xbc,0x03,0xc1,0x03,0x19,0x1f};
    memcpy(b.data()+off,magic,8);put(b,off+32,128);
    assert(zt_dump_plain_bundle(b.data(),b.size()));
    assert(zt_dump_plain_bundle(b.data()+off,128));
    assert(!zt_dump_plain_bundle(b.data(),b.size()-1));
    put(b,off+32,0xffffffff);assert(!zt_dump_plain_bundle(b.data(),b.size()));
    put(b,28,0xffffffff);assert(!zt_dump_plain_bundle(b.data(),b.size()));
  }
  for(int i=1;i<argc;++i) {
    std::ifstream f(argv[i],std::ios::binary);assert(f.good());
    std::vector<unsigned char>b((std::istreambuf_iterator<char>(f)),{});
    assert(!zt_dump_plain_bundle(b.data(),b.size()));
  }
}
