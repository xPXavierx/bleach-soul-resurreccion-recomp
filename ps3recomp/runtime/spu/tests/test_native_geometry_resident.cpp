#include "native_geometry_resident.h"
#include <array>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d\n",__LINE__);abort();}}while(0)
int main(){
    unsigned input[3]={3,7,11},output[3]={9,49,121},restored[3]={};
    ps3_geo_resident_input in[]={{input,sizeof(input)}};
    ps3_geo_resident_output out[]={{output,sizeof(output)}},dst[]={{restored,sizeof(restored)}};
    int hit=0;auto* first=ps3_geo_resident_acquire(in,1,out,1,&hit);CHECK(first&&!hit);
    auto* concurrent=ps3_geo_resident_acquire(in,1,out,1,&hit);CHECK(concurrent&&!hit);
    CHECK(!ps3_geo_resident_restore(first,dst,1));ps3_geo_resident_release(concurrent);
    CHECK(ps3_geo_resident_publish(first,out,1));
    auto* second=ps3_geo_resident_acquire(in,1,out,1,&hit);CHECK(second&&hit);
    CHECK(ps3_geo_resident_restore(second,dst,1));CHECK(!memcmp(output,restored,sizeof(output)));
    ++input[2];auto* changed=ps3_geo_resident_acquire(in,1,out,1,&hit);CHECK(changed&&!hit);
    ps3_geo_resident_release(changed);--input[2];
    ps3_geo_resident_input split[]={{input,4},{input+1,8}};
    changed=ps3_geo_resident_acquire(split,2,out,1,&hit);CHECK(changed&&!hit);ps3_geo_resident_release(changed);
    // Keep leases pinned while other workers force replacements and mutations.
    std::vector<std::thread> workers;
    for(unsigned t=0;t<8;t++)workers.emplace_back([t]{
        std::array<unsigned,128> key{},value{},copy{};
        for(unsigned j=0;j<6000;j++){
            for(unsigned k=0;k<128;k++){key[k]=(j%4096)*1009+k;value[k]=key[k]^0xa5f01234;}
            key[0]^=t%2;value[0]=key[0]^0xa5f01234;
            ps3_geo_resident_input i[]={{key.data(),sizeof(key)}};
            ps3_geo_resident_output o[]={{value.data(),sizeof(value)}},d[]={{copy.data(),sizeof(copy)}};
            int h;auto* lease=ps3_geo_resident_acquire(i,1,o,1,&h);CHECK(lease);
            if(!h)CHECK(ps3_geo_resident_publish(lease,o,1));
            CHECK(ps3_geo_resident_restore(lease,d,1));CHECK(value==copy);ps3_geo_resident_release(lease);
        }
    });for(auto& thread:workers)thread.join();
    CHECK(ps3_geo_resident_restore(first,dst,1));CHECK(!memcmp(output,restored,sizeof(output)));
    ps3_geo_resident_release(first);ps3_geo_resident_release(second);
    ps3_geo_resident_stats stats;ps3_geo_resident_get_stats(&stats);
    CHECK(stats.hits&&stats.evicted&&stats.bytes<=128*1024*1024);
    printf("PASS: exact inputs, span boundaries, unpublished isolation, 48000 concurrent uses, pinned lifetime, eviction, bounded storage (%llu bytes)\n",(unsigned long long)stats.bytes);
    ps3_geo_resident_reset();ps3_geo_resident_get_stats(&stats);CHECK(!stats.bytes);
    return 0;
}
