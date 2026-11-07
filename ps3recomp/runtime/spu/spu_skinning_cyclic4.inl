/* Instantiate checked or proven-bounded arithmetic over the same data contract. */
static void PS3_SKIN4_RUN(const ps3_skin4_batch* batch) {
    u128 scale;for(unsigned i=0;i<4;i++)scale._u32[i]=0x3b808080;
    for(unsigned v=0;v<batch->vertices;v++) {
        const uint8_t* influence=batch->influences+v*8;
        u128 weights;for(unsigned i=0;i<4;i++)weights._f32[i]=(float)influence[i*2];
        weights=PS3_SKIN4_MUL(weights,scale);
        const u128 w0=ps3_skin4_splat(weights,0),w1=ps3_skin4_splat(weights,1),
                   w2=ps3_skin4_splat(weights,2),w3=ps3_skin4_splat(weights,3);
        const uint8_t *p0=batch->palette+influence[1]*48,*p1=batch->palette+influence[3]*48,
                      *p2=batch->palette+influence[5]*48,*p3=batch->palette+influence[7]*48;
        u128 rows[3];
        for(unsigned row=0;row<3;row++) {
            u128 sum=PS3_SKIN4_MUL(w0,ps3_skin4_load(p0+row*16));
            sum=PS3_SKIN4_MADD(w1,ps3_skin4_load(p1+row*16),sum);
            sum=PS3_SKIN4_MADD(w2,ps3_skin4_load(p2+row*16),sum);
            rows[row]=PS3_SKIN4_MADD(w3,ps3_skin4_load(p3+row*16),sum);
        }
        u128 translation;for(unsigned i=0;i<3;i++)translation._u32[i]=rows[i]._u32[3];
        translation._u32[3]=0;rows[0]._u32[3]=0x3f800000;rows[1]._u32[3]=rows[2]._u32[3]=0;
        const u128 position=ps3_skin4_load(batch->positions+v*16);
        u128 yzxw,zxyw;
        for(unsigned i=0;i<3;i++){yzxw._u32[i]=position._u32[(i+1)%3];zxyw._u32[i]=position._u32[(i+2)%3];}
        yzxw._u32[3]=zxyw._u32[3]=position._u32[3];
        u128 result=PS3_SKIN4_MADD(position,rows[0],translation);
        result=PS3_SKIN4_MADD(yzxw,rows[1],result);
        result=PS3_SKIN4_MADD(zxyw,rows[2],result);
        for(unsigned row=0;row<3;row++)ps3_skin4_store(batch->frames+v*48+row*16,rows[row]);
        ps3_skin4_store(batch->positions+v*16,result);
    }
}
