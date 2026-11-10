// Synthetic caller of the actual runtime adapter. No game, firmware or GPU.
static void encode(const uint8_t *data) {
    char hex[177];
    for (int i=0;i<88;i++) snprintf(hex+2*i,3,"%02x",data[i]);
    assert(!_putenv_s("MK8_MII_CHARINFO",hex));
}

int main(int argc,char **argv) {
    assert(argc==2);
    unsigned cycles=(unsigned)strtoul(argv[1],NULL,10);
    assert(cycles && cycles<=1000000);
    uint8_t profile[88], actual[88];
    mii_default_charinfo(profile,"Simulation");
    encode(profile);
    assert(mk8_profile_charinfo(actual) && !memcmp(actual,profile,88));
    unsigned requests=0;
    // All lengths around each record boundary, both layouts and source filters.
    for (unsigned stride=88;stride<=92;stride+=4) {
        for (unsigned sources=0;sources<4;sources++) {
            for (unsigned local=0;local<2;local++) {
                unsigned total=((sources&1)&&local?1:0)+(sources&2?6:0);
                assert(mk8_mii_count(sources,local)==total);
                for (unsigned capacity=0;capacity<=7*stride+1;capacity++) {
                    uint8_t guarded[16+7*92+1+16];
                    memset(guarded,0xa5,sizeof guarded);
                    unsigned count=mk8_mii_get(guarded+16,capacity,stride,sources,local);
                    unsigned expected=capacity/stride;
                    if(expected>total) expected=total;
                    assert(count==expected);
                    for(unsigned i=0;i<16;i++) assert(guarded[i]==0xa5);
                    for(unsigned i=16+count*stride;i<sizeof guarded;i++) assert(guarded[i]==0xa5);
                    if(count && (sources&1) && local) assert(!memcmp(guarded+16,profile,88));
                    requests++;
                }
            }
        }
    }
    assert(!mk8_mii_get(NULL,1024,88,3,true));
    uint8_t records[7*92]; memset(records,0xa5,sizeof records);
    assert(!mk8_mii_get(records,sizeof records,89,3,true) && records[0]==0xa5);
    // Every appearance byte must reject 255, without partially writing output.
    for(unsigned offset=38;offset<88;offset++) {
        uint8_t bad[88];memcpy(bad,profile,88);bad[offset]=255;encode(bad);
        memset(actual,0xa5,sizeof actual);
        assert(!mk8_profile_charinfo(actual));
        for(unsigned j=0;j<88;j++) assert(actual[j]==0xa5);
    }
    // Repeated menu/race/return-style queries; labels describe synthetic workload only.
    for(unsigned cycle=0;cycle<cycles;cycle++) {
        profile[15]=(uint8_t)cycle;
        profile[0x27]=(uint8_t)(cycle%12);
        encode(profile);
        for(unsigned phase=0;phase<3;phase++) {
            assert(mk8_mii_get(records,sizeof records,92,3,true)==7);
            assert(!memcmp(records,profile,88));
            for(unsigned i=1;i<7;i++) assert(records[i*92+88]==1);
            requests++;
        }
    }
    printf("{\"cycles\":%u,\"requests\":%u,\"invalid_fields_rejected\":50,\"gameplay_verified\":false}\n",cycles,requests);
    return 0;
}
