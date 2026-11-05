float4 OKStatistics(float2 pos) {
    // Uniform stratified sampling, with bounded log luminance and highlight
    // rejection for white balance. The image is never copied back to the CPU.
    float4 total=0;
    [unroll] for(int y=0;y<4;++y) [unroll] for(int x=0;x<4;++x) {
        float2 uv=pos+(float2(x,y)-1.5)/float2(64,36);
        float3 c=OKRead(INPUT.SampleLevel(linearSampler,uv,0).rgb);
        float l=max(OKLuma(c),1e-6);
        float3 ratios=clamp(log2(max(c,1e-6)/l),-2,2);
        // Bright colored lamps and deep noise have little white-balance weight.
        float w=smoothstep(.005,.05,l)*(1-smoothstep(1,4,l));
        total+=float4(clamp(log2(l),-12,4),ratios*w);
    }
    return total/16;
}
