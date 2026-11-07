// Experimental GPU visibility contract, not enabled in the game.
// One stable triangle slot per input; rejected slots become degenerate.
// Production need not read back a surviving count. Debug mask follows indices.
ByteAddressBuffer input : register(t0);
RWByteAddressBuffer output : register(u0);
float4 f4(uint offset) { return asfloat(input.Load4(offset)); }
float3 project(uint index, out uint flags) {
    float3 p=f4(input.Load(4)+index*16).xyz;
    float4 w0=f4(16),w1=f4(32),w2=f4(48),clip;
    [unroll] for(uint r=0;r<4;r++) {
        float4 q=f4(64+r*16);
        precise float4 t=mad(q.w,float4(0,0,0,1),mad(q.z,w2,mad(q.y,w1,q.x*w0)));
        clip[r]=mad(p.z,t.z,mad(p.y,t.y,mad(p.x,t.x,t.w)));
    }
    precise float3 screen=mad(clip.xyz/abs(clip.w),f4(128).xyz,f4(144).xyz);
    float3 lo=f4(160).xyz,hi=f4(176).xyz;
    flags=clip.w>0?2:0;
    [unroll] for(uint k=0;k<3;k++) {
        if(screen[k]>hi[k])flags|=128u>>(k*2);
        if(lo[k]>screen[k])flags|=64u>>(k*2);
    }
    return screen;
}
int quantize(float x) {
    if(isnan(x))return 0;
    if(x>=2147483648.0)return 2147483647;
    if(x<=-2147483648.0)return asint(0x80000000);
    return (int)x;
}
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID) {
    uint triangles=input.Load(0),t=id.x;if(t>=triangles)return;
    uint3 indices=input.Load3(input.Load(8)+t*12);
    uint a,b,c;float3 p=project(indices.x,a),q=project(indices.y,b),r=project(indices.z,c);
    uint common=a&b&c,any=a|b|c;
    bool reject=(common&252)!=0;
    uint settings=input.Load(12),cull=settings&3;
    if((common&2)!=0 && (any&4)==0) {
        precise float area=(q.x-p.x)*(r.y-p.y)-(q.y-p.y)*(r.x-p.x);
        if(cull==2)area=-area;
        if(cull!=0 && area>0)reject=true;
        float scale=(settings&4)!=0?2.0:1.0;
        int3 x=int3(quantize(mad(p.x,scale,0.5)),quantize(mad(q.x,scale,0.5)),quantize(mad(r.x,scale,0.5)));
        int3 y=int3(quantize(mad(p.y,scale,0.5)),quantize(mad(q.y,scale,0.5)),quantize(mad(r.y,scale,0.5)));
        if((x.x==x.y && x.y==x.z)||(y.x==y.y && y.y==y.z))reject=true;
    }
    output.Store3(t*12,reject?uint3(0,0,0):indices);
    output.Store(triangles*12+t*4,reject?0:1);
}
